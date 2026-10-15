#pragma once

// A minimal, READ-ONLY reader for osu!lazer's client.realm, just enough to list every beatmap
// set's files: for each BeatmapSet, the (Filename -> SHA-256 hash) pairs of its Files list. The
// hash names the file in lazer's store: <data>/files/<h[0]>/<h[0..1]>/<hash>.
//
// The Realm file format is undocumented; this follows what the file itself shows (format 24,
// verified against a live lazer database 2026-10-10):
//   * header: two 8-byte top refs, "T-DB", format bytes, flags (bit 0 picks the live top ref).
//   * every node is an array: 8-byte header "AAAA" + flags + 24-bit big-endian size. flags:
//     0x80 inner B+tree node, 0x40 has refs, 0x20 context flag, (f>>3)&3 width type,
//     width = (1 << (f & 7)) >> 1 bits (multiply-type arrays: bytes per element).
//   * group top: [0] table names (strings), [1] table refs.
//   * table top: [0] spec ([1] column names, [5] column keys: low 16 bits = leaf index),
//     [2] cluster tree root.
//   * cluster inner node: [0] child key offsets (0 = compact: i << (depth*8)), [1] tagged depth,
//     [2] tagged subtree size, [3..] children. Leaf: [0] keys (tagged count = compact 0..n-1, or
//     a key array), [1 + leaf index] = column data.
//   * strings: no refs = small (fixed width, last byte = padding; padding == width means null);
//     refs without context flag = medium ([0] offsets, [1] blob); refs WITH context flag = big
//     (each element a ref to its own blob, 0 = null). Blobs end with a NUL.
//   * a single link stores target key + 1 (0 = null); a link LIST stores raw keys.
// Anything that does not look like that fails safe: the reader returns false and lazer is
// simply not listed. It never writes, and reads an in-memory copy of the file.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Filmmaker {
namespace OsuRealm {

struct NamedFile { std::string name, hash; };
struct BeatmapSetFiles { std::vector<NamedFile> files; };
// A lazer skin: its name, creator, ID (lower-case hex of the 16 stored bytes, "" when unreadable)
// and its files (skin.ini, hitcircle@2x.png, normal-hitnormal.wav, ...).
struct SkinFiles { std::string name, creator, idHex; std::vector<NamedFile> files; };

class Reader {
public:
	explicit Reader(const std::vector<uint8_t>& data) : b(data) {}

	bool ReadSets(std::vector<BeatmapSetFiles>& out, std::string* err = nullptr) {
		try { return Read(out); }
		catch (const char* e) { if (err) *err = e; return false; }
	}

	bool ReadSkins(std::vector<SkinFiles>& out, std::string* err = nullptr) {
		try {
			Table skins;
			if (!FindTable("class_Skin", skins)) throw "realm: no Skin table";
			const auto usage = Usage();
			for (const Leaf& l : skins.leaves) {
				const auto names = Strings((uint64_t)Column(l, skins, "Name"));
				std::vector<std::string> creators, ids;
				if (skins.cols.count("Creator")) creators = Strings((uint64_t)Column(l, skins, "Creator"));
				// The ID is a UUID column; read as a binary blob it is the 16 raw bytes. Optional: a
				// layout this does not understand just leaves the ID empty (the skin is matched by name).
				if (skins.cols.count("ID")) { try { ids = Fixed16((uint64_t)Column(l, skins, "ID"), l.keys.size()); } catch (const char*) { ids.clear(); } }
				const int64_t filesCol = Column(l, skins, "Files");
				int64_t deleteCol = 0;
				if (skins.cols.count("DeletePending")) deleteCol = Column(l, skins, "DeletePending");
				for (uint32_t i = 0; i < l.keys.size() && i < names.size(); ++i) {
					if (deleteCol && IsRef(deleteCol) && Get((uint64_t)deleteCol, i) != 0) continue;
					SkinFiles sk;
					sk.name = names[i];
					if (i < creators.size()) sk.creator = creators[i];
					if (i < ids.size() && ids[i].size() >= 16) {
						static const char* hx = "0123456789abcdef";
						for (size_t k = 0; k < 16; ++k) { const uint8_t c = (uint8_t)ids[i][k]; sk.idHex += hx[c >> 4]; sk.idHex += hx[c & 15]; }
					}
					const int64_t list = Get((uint64_t)filesCol, i);
					if (IsRef(list))
						for (int64_t k : ListKeys((uint64_t)list)) {
							auto u = usage.find(k);
							if (u != usage.end()) sk.files.push_back(u->second);
						}
					out.push_back(std::move(sk));
				}
			}
			return true;
		} catch (const char* e) { if (err) *err = e; return false; }
	}

	// Lazer's computed star rating per difficulty, keyed by the .osu file's SHA-256 (its store
	// name) and by its MD5 (what a stable copy of the same file hashes to).
	bool ReadStars(std::map<std::string, double>& bySha, std::map<std::string, double>& byMd5, std::string* err = nullptr) {
		try {
			Table t;
			if (!FindTable("class_Beatmap", t)) throw "realm: no Beatmap table";
			for (const Leaf& l : t.leaves) {
				const auto sha = Strings((uint64_t)Column(l, t, "Hash"));
				const auto md5 = Strings((uint64_t)Column(l, t, "MD5Hash"));
				const auto sr = Doubles((uint64_t)Column(l, t, "StarRating"));
				for (size_t i = 0; i < l.keys.size() && i < sha.size() && i < md5.size() && i < sr.size(); ++i) {
					if (!(sr[i] > 0.0)) continue;
					if (!sha[i].empty()) bySha[sha[i]] = sr[i];
					if (!md5[i].empty()) byMd5[md5[i]] = sr[i];
				}
			}
			return true;
		} catch (const char* e) { if (err) *err = e; return false; }
	}

private:
	const std::vector<uint8_t>& b;

	struct Hdr { bool inner, refs, ctx; int wtype, width; uint32_t size; };

	void Need(uint64_t off, uint64_t len) const {
		if (off > b.size() || len > b.size() - off) throw "realm: out of bounds";
	}
	Hdr H(uint64_t r) const {
		Need(r, 8);
		if (std::memcmp(&b[r], "AAAA", 4) != 0) throw "realm: bad node";
		const uint8_t f = b[r + 4];
		Hdr h;
		h.inner = (f & 0x80) != 0; h.refs = (f & 0x40) != 0; h.ctx = (f & 0x20) != 0;
		h.wtype = (f & 0x18) >> 3; h.width = (1 << (f & 7)) >> 1;
		h.size = (uint32_t(b[r + 5]) << 16) | (uint32_t(b[r + 6]) << 8) | b[r + 7];
		return h;
	}
	int64_t Get(uint64_t r, uint32_t i) const {
		const Hdr h = H(r);
		if (i >= h.size) throw "realm: index";
		const uint64_t d = r + 8;
		switch (h.width) {
		case 0: return 0;
		case 64: { Need(d + 8ull * i, 8); int64_t v; std::memcpy(&v, &b[d + 8ull * i], 8); return v; }
		case 32: { Need(d + 4ull * i, 4); int32_t v; std::memcpy(&v, &b[d + 4ull * i], 4); return v; }
		case 16: { Need(d + 2ull * i, 2); int16_t v; std::memcpy(&v, &b[d + 2ull * i], 2); return v; }
		case 8: { Need(d + i, 1); return (int8_t)b[d + i]; }
		default: {
			const uint64_t bit = uint64_t(i) * h.width;
			Need(d + bit / 8, 1);
			return (b[d + bit / 8] >> (bit % 8)) & ((1 << h.width) - 1);
		}
		}
	}
	std::vector<int64_t> Arr(uint64_t r) const {
		const Hdr h = H(r);
		std::vector<int64_t> v(h.size);
		for (uint32_t i = 0; i < h.size; ++i) v[i] = Get(r, i);
		return v;
	}
	// Cluster key arrays are realm ArrayUnsigned: an 8/16/32-bit width holds 0..255 etc., NOT a
	// signed value (reading them signed turned key 200 into -56 and misnamed every later file).
	std::vector<int64_t> KeyArr(uint64_t r) const {
		const Hdr h = H(r);
		std::vector<int64_t> v(h.size);
		for (uint32_t i = 0; i < h.size; ++i) {
			const int64_t x = Get(r, i);
			switch (h.width) {
			case 8: v[i] = x & 0xFF; break;
			case 16: v[i] = x & 0xFFFF; break;
			case 32: v[i] = x & 0xFFFFFFFFll; break;
			default: v[i] = x;
			}
		}
		return v;
	}
	static bool IsRef(int64_t v) { return v > 0 && (v & 1) == 0; }

	std::vector<std::string> Strings(uint64_t r) const {
		const Hdr h = H(r);
		std::vector<std::string> out;
		out.reserve(h.size);
		if (!h.refs) {
			const uint32_t w = (uint32_t)h.width;
			Need(r + 8, uint64_t(w) * h.size);
			for (uint32_t i = 0; i < h.size; ++i) {
				if (w == 0) { out.emplace_back(); continue; }
				const uint8_t* s = &b[r + 8 + uint64_t(i) * w];
				const uint32_t pad = s[w - 1];
				out.emplace_back(pad >= w ? std::string() : std::string((const char*)s, w - 1 - pad));
			}
			return out;
		}
		if (!h.ctx) {
			const int64_t offsRef = Get(r, 0), blob = Get(r, 1);
			if (!IsRef(offsRef) || !IsRef(blob)) throw "realm: medium strings";
			const auto offs = Arr((uint64_t)offsRef);
			int64_t prev = 0;
			for (int64_t o : offs) {
				if (o < prev) throw "realm: offsets";
				Need((uint64_t)blob + 8 + prev, (uint64_t)(o - prev));
				const char* s = (const char*)&b[(uint64_t)blob + 8 + prev];
				const size_t len = (size_t)(o - prev);
				out.emplace_back(s, len > 0 ? len - 1 : 0);
				prev = o;
			}
			return out;
		}
		for (uint32_t i = 0; i < h.size; ++i) {
			const int64_t ref = Get(r, i);
			if (!IsRef(ref)) { out.emplace_back(); continue; }
			const Hdr bh = H((uint64_t)ref);
			Need((uint64_t)ref + 8, bh.size);
			out.emplace_back((const char*)&b[(uint64_t)ref + 8], bh.size > 0 ? bh.size - 1 : 0);
		}
		return out;
	}

	// A UUID column leaf: a byte array holding a small prefix (a null bitmap, ceil(n/8) bytes, measured
	// 2026-10-10: 162 bytes for 10 skins) followed by the 16 RFC-order bytes of each element.
	std::vector<std::string> Fixed16(uint64_t r, size_t count) const {
		const Hdr h = H(r);
		if (h.refs || h.wtype != 1 || h.width != 1 || h.size < 16ull * count) throw "realm: uuid layout";
		const uint64_t prefix = h.size - 16ull * count;
		if (prefix != (count + 7) / 8) throw "realm: uuid prefix";
		Need(r + 8, h.size);
		std::vector<std::string> v;
		for (size_t i = 0; i < count; ++i) v.emplace_back((const char*)&b[r + 8 + prefix + 16ull * i], 16);
		return v;
	}

	// A double column leaf: raw little-endian doubles (multiply-width array, 8 bytes each).
	std::vector<double> Doubles(uint64_t r) const {
		const Hdr h = H(r);
		std::vector<double> v(h.size, 0.0);
		if (h.width != 8 && h.width != 64) throw "realm: double width";
		Need(r + 8, 8ull * h.size);
		for (uint32_t i = 0; i < h.size; ++i) std::memcpy(&v[i], &b[r + 8 + 8ull * i], 8);
		return v;
	}

	struct Leaf { uint64_t node; std::vector<int64_t> keys; };
	void Leaves(uint64_t node, int64_t offset, std::vector<Leaf>& out, int guard = 0) const {
		if (guard > 16) throw "realm: depth";
		const Hdr h = H(node);
		if (h.inner) {
			if (h.size < 3) throw "realm: inner";
			const int64_t keysRef = Get(node, 0);
			const int64_t depth = Get(node, 1) >> 1;
			const uint32_t n = h.size - 3;
			std::vector<int64_t> keys;
			if (IsRef(keysRef)) keys = KeyArr((uint64_t)keysRef);
			else for (uint32_t i = 0; i < n; ++i) keys.push_back(int64_t(i) << (depth * 8));
			if (keys.size() < n) throw "realm: inner keys";
			for (uint32_t i = 0; i < n; ++i) {
				const int64_t child = Get(node, 3 + i);
				if (!IsRef(child)) throw "realm: child";
				Leaves((uint64_t)child, offset + keys[i], out, guard + 1);
			}
			return;
		}
		Leaf l; l.node = node;
		const int64_t k0 = Get(node, 0);
		if (k0 & 1) { for (int64_t i = 0; i < (k0 >> 1); ++i) l.keys.push_back(offset + i); }
		else for (int64_t k : KeyArr((uint64_t)k0)) l.keys.push_back(offset + k);
		out.push_back(std::move(l));
	}

	struct Table { std::map<std::string, int> cols; std::vector<Leaf> leaves; };
	bool FindTable(const char* name, Table& t) const {
		uint64_t top0, top1;
		Need(0, 24);
		std::memcpy(&top0, &b[0], 8); std::memcpy(&top1, &b[8], 8);
		if (std::memcmp(&b[16], "T-DB", 4) != 0) throw "realm: not a realm file";
		const uint64_t top = (b[23] & 1) ? top1 : top0;
		const int64_t namesRef = Get(top, 0), tablesRef = Get(top, 1);
		if (!IsRef(namesRef) || !IsRef(tablesRef)) throw "realm: group";
		const auto names = Strings((uint64_t)namesRef);
		const auto tables = Arr((uint64_t)tablesRef);
		for (size_t i = 0; i < names.size() && i < tables.size(); ++i) {
			if (names[i] != name) continue;
			if (!IsRef(tables[i])) throw "realm: table ref";
			const int64_t spec = Get((uint64_t)tables[i], 0), root = Get((uint64_t)tables[i], 2);
			if (!IsRef(spec) || !IsRef(root)) throw "realm: table";
			const auto cnames = Strings((uint64_t)Get((uint64_t)spec, 1));
			const auto ckeys = Arr((uint64_t)Get((uint64_t)spec, 5));
			for (size_t c = 0; c < cnames.size() && c < ckeys.size(); ++c) t.cols[cnames[c]] = int(ckeys[c] & 0xFFFF);
			Leaves((uint64_t)root, 0, t.leaves);
			return true;
		}
		return false;
	}
	int64_t Column(const Leaf& l, const Table& t, const char* name) const {
		auto it = t.cols.find(name);
		if (it == t.cols.end()) throw "realm: missing column";
		const int64_t ref = Get(l.node, 1 + it->second);
		return ref;
	}

	// RealmNamedFileUsage key -> (Filename, File.Hash).
	std::map<int64_t, NamedFile> Usage() const {
		Table files, named;
		if (!FindTable("class_File", files) || !FindTable("class_RealmNamedFileUsage", named)) throw "realm: tables missing";
		std::map<int64_t, std::string> hashes;
		for (const Leaf& l : files.leaves) {
			const auto hs = Strings((uint64_t)Column(l, files, "Hash"));
			for (size_t i = 0; i < l.keys.size() && i < hs.size(); ++i) hashes[l.keys[i]] = hs[i];
		}
		std::map<int64_t, NamedFile> usage;
		for (const Leaf& l : named.leaves) {
			const auto links = Arr((uint64_t)Column(l, named, "File"));
			const auto names = Strings((uint64_t)Column(l, named, "Filename"));
			for (size_t i = 0; i < l.keys.size() && i < links.size() && i < names.size(); ++i) {
				if (links[i] <= 0) continue; // null link
				auto h = hashes.find(links[i] - 1);
				if (h != hashes.end()) usage[l.keys[i]] = { names[i], h->second };
			}
		}
		return usage;
	}

	// The keys of a link list: a long list is a B+tree of keys, a short one a single leaf.
	std::vector<int64_t> ListKeys(uint64_t list) const {
		std::vector<int64_t> keys;
		const Hdr lh = H(list);
		if (lh.inner) {
			std::vector<uint64_t> stack{ list };
			int guard = 0;
			while (!stack.empty() && ++guard < 100000) {
				const uint64_t n = stack.back(); stack.pop_back();
				const Hdr nh = H(n);
				if (!nh.inner) { for (int64_t k : Arr(n)) keys.push_back(k); continue; }
				// inner: [0] offsets/size info, children follow; push in reverse for order
				for (uint32_t c = nh.size; c-- > 1;) { const int64_t ch = Get(n, c); if (IsRef(ch)) stack.push_back((uint64_t)ch); }
			}
		} else keys = Arr(list);
		return keys;
	}

	bool Read(std::vector<BeatmapSetFiles>& out) {
		Table sets;
		if (!FindTable("class_BeatmapSet", sets)) throw "realm: tables missing";
		const auto usage = Usage();
		for (const Leaf& l : sets.leaves) {
			const int64_t filesCol = Column(l, sets, "Files");
			int64_t deleteCol = 0;
			if (sets.cols.count("DeletePending")) deleteCol = Column(l, sets, "DeletePending");
			for (uint32_t i = 0; i < l.keys.size(); ++i) {
				if (deleteCol && IsRef(deleteCol) && Get((uint64_t)deleteCol, i) != 0) continue;
				const int64_t list = Get((uint64_t)filesCol, i);
				if (!IsRef(list)) continue;
				BeatmapSetFiles set;
				for (int64_t k : ListKeys((uint64_t)list)) {
					auto u = usage.find(k);
					if (u != usage.end()) set.files.push_back(u->second);
				}
				if (!set.files.empty()) out.push_back(std::move(set));
			}
		}
		return true;
	}
};

} // namespace OsuRealm
} // namespace Filmmaker
