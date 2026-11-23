#pragma once

// True 3D DXBC Vertex-Shader patching for WorldWarp (Inception, Rolling Horizon, World Wave,
// World Fold). Splices a D3DCompile-generated SM5 world-space deformation snippet into CS2's
// own 3D world vertex shaders right before `ret`, reading CS2's live `PerViewConstantBuffer_t`
// (`g_matWorldToProjection` at c0..c3, `g_matProjectionToWorld` at c4..c7) and WorldWarp
// parameters in `cb13[0..6]`. Both depth pre-pass and forward color pass bend the exact same
// 3D vertices — zero pixel stretching, zero disocclusion curtains, native textures & lighting.

#include "../Environment/EnvironmentDxbcHash.h"
#include "../Relight/RelightIrradianceMath.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Filmmaker::WorldWarpDxbc {

using Words = Relight::IrradianceDxbc::Words;
using Operand = Relight::IrradianceDxbc::Operand;

constexpr unsigned kSnippetNativeViewSlot = 12; // placeholder slot in WarpSnippetVS, rewritten to perViewSlot
constexpr unsigned kWarpConstantSlot = 13;      // live cb13[0..6] bound during WorldWarp draws
constexpr unsigned kWarpConstantRows = 7;

// SM4/SM5 opcode numbers (from d3d11TokenizedProgramFormat.hpp).
constexpr unsigned kOpRet = 62;
constexpr unsigned kOpCustomData = 53;
constexpr unsigned kOpDclConstantBuffer = 89;
constexpr unsigned kOpDclOutputSiv = 103;
constexpr unsigned kOpDclTemps = 104;
constexpr unsigned kOpDclIndexableTemp = 105;
constexpr unsigned kOpDclGlobalFlags = 106;
constexpr unsigned kSbNamePosition = 1; // D3D10_SB_NAME_POSITION

struct WarpSnippet {
	Words body;
	unsigned tempCount = 0;
	bool valid = false;
};

inline bool IsDeclarationOpcode(unsigned op) {
	// DCL_RESOURCE (88) .. DCL_GLOBAL_FLAGS (106), plus SM5 DCL_STREAM..DCL_THREAD_GROUP_SHARED_MEMORY (141..164)
	return (op >= 88 && op <= 106) || (op >= 141 && op <= 165);
}

inline uint32_t ReadU32(const uint8_t* p, size_t at) {
	uint32_t v = 0;
	std::memcpy(&v, p + at, 4);
	return v;
}

inline bool ExtractProgramChunk(const void* data, size_t size, size_t& outChunkAt, Words& outCode) {
	outChunkAt = 0;
	outCode.clear();
	if (!data || size < 32 || size > 16u * 1024u * 1024u)
		return false;
	const auto* p = static_cast<const uint8_t*>(data);
	if (ReadU32(p, 0) != 0x43425844u || ReadU32(p, 24) != size) // 'DXBC'
		return false;
	const unsigned chunks = ReadU32(p, 28);
	if (chunks == 0 || chunks > (size - 32) / 4)
		return false;
	for (unsigned i = 0; i < chunks; ++i) {
		const size_t at = ReadU32(p, 32 + i * 4);
		if (at < 32 + chunks * 4 || at > size - 8)
			return false;
		const uint32_t tag = ReadU32(p, at);
		const uint32_t len = ReadU32(p, at + 4);
		if (len > size - at - 8)
			return false;
		if (tag == 0x52444853u || tag == 0x58454853u) { // 'SHDR' or 'SHEX'
			if (outChunkAt || len < 8 || (len % 4) != 0)
				return false;
			outChunkAt = at;
			outCode.resize(len / 4);
			std::memcpy(outCode.data(), p + at + 8, len);
		}
	}
	if (!outChunkAt || outCode.size() < 2 || outCode[1] != outCode.size())
		return false;
	return true;
}

// Finds the word index of the first immediate dimension index inside an operand (skipping
// any extended operand modifier tokens). Returns 0 if the operand has no 1D/2D immediate index.
inline size_t OperandIndexWord(const Words& code, const Operand& o) {
	size_t idx = o.start + 1;
	uint32_t ext = code[o.start];
	while (ext & 0x80000000u) {
		if (idx >= o.end) return 0;
		ext = code[idx++];
	}
	return idx < o.end ? idx : 0;
}

// Extracts the executable instruction body from a D3DCompile-compiled WarpSnippetVS (`vs_5_0`).
// Requirements on WarpSnippetVS:
//   - Input v0 = float4 clipIn (SV_Position from the original shader)
//   - Output o0 = float4 outPos (SV_Position)
//   - cb12 = NativeView (c0..c3 worldToProj, c4..c7 projToWorld)
//   - cb13 = WorldWarpCB (c0..c6)
//   - Single return at the end, no immediate constant buffers or indexable temps.
inline bool ExtractWarpSnippet(const void* data, size_t size, WarpSnippet& out) {
	out = {};
	size_t chunkAt = 0;
	Words code;
	if (!ExtractProgramChunk(data, size, chunkAt, code))
		return false;
	if ((code[0] >> 16) != 1) // must be Vertex Shader
		return false;

	size_t bodyStart = 0;
	size_t bodyEnd = 0;
	for (size_t at = 2; at < code.size();) {
		const unsigned op = code[at] & 2047u;
		size_t count = (code[at] >> 24) & 127u;
		if (op == kOpCustomData)
			return false; // snippet must not contain immediate constant buffers
		if (count == 0 || count > code.size() - at)
			return false;
		const size_t end = at + count;
		if (op == kOpDclIndexableTemp)
			return false;
		if (op == kOpDclTemps) {
			if (count != 2) return false;
			out.tempCount = code[at + 1];
		} else if (IsDeclarationOpcode(op)) {
			// skip other declarations
		} else {
			if (bodyStart == 0) bodyStart = at;
			if (op == kOpRet) {
				if (end != code.size()) return false; // snippet must have a single ret at the end
				bodyEnd = at;
				break;
			}
		}
		at = end;
	}
	if (bodyStart == 0 || bodyEnd <= bodyStart || out.tempCount == 0)
		return false;
	out.body.assign(code.begin() + bodyStart, code.begin() + bodyEnd);
	out.valid = true;
	return true;
}

// Relocates the extracted snippet body for a specific target CS2 Vertex Shader:
//   v0   -> r[savedPosTemp]
//   r[t] -> r[snippetTempBase + t]
//   o0   -> o[posReg]
//   cb12 -> cb[perViewSlot]
inline bool RelocateSnippetBody(const WarpSnippet& snippet, unsigned savedPosTemp,
	unsigned snippetTempBase, unsigned posReg, unsigned perViewSlot, Words& outBody) {
	outBody = snippet.body;
	for (size_t at = 0; at < outBody.size();) {
		const unsigned op = outBody[at] & 2047u;
		const size_t count = (outBody[at] >> 24) & 127u;
		if (count == 0 || count > outBody.size() - at)
			return false;
		const size_t end = at + count;
		size_t cursor = at;
		uint32_t ext = outBody[cursor];
		while (ext & 0x80000000u) {
			if (++cursor >= end) return false;
			ext = outBody[cursor];
		}
		++cursor;

		while (cursor < end) {
			Operand o;
			if (!Relight::IrradianceDxbc::ReadOperand(outBody, cursor, end, o))
				return false;
			const size_t idxWord = OperandIndexWord(outBody, o);
			if (o.type == 0) { // TEMP r[t]
				if (!idxWord) return false;
				outBody[idxWord] = snippetTempBase + outBody[idxWord];
			} else if (o.type == 1) { // INPUT v0 -> TEMP r[savedPosTemp]
				if (!idxWord || o.index != 0) return false;
				outBody[o.start] = (outBody[o.start] & ~(0xFFu << 12)) | (0u << 12);
				outBody[idxWord] = savedPosTemp;
			} else if (o.type == 2) { // OUTPUT o0 -> OUTPUT o[posReg]
				if (!idxWord || o.index != 0) return false;
				outBody[idxWord] = posReg;
			} else if (o.type == 8) { // CONSTANT_BUFFER cb[slot][row]
				if (!idxWord) return false;
				if (outBody[idxWord] == kSnippetNativeViewSlot)
					outBody[idxWord] = perViewSlot;
			}
		}
		at = end;
	}
	return true;
}

// Patches a native CS2 3D Vertex Shader DXBC container so its SV_Position output is deformed
// by `snippet` right before every `ret`. Preserves all other outputs (normals, UVs, world pos,
// tangent basis, color) and re-signs the DXBC retail MD5 checksum.
inline bool PatchVertexShaderDxbc(const void* origData, size_t origSize,
	unsigned perViewSlot, const WarpSnippet& snippet, std::vector<uint8_t>& outDxbc) {
	outDxbc.clear();
	if (!snippet.valid || perViewSlot >= kWarpConstantSlot)
		return false;

	size_t programChunkAt = 0;
	Words code;
	if (!ExtractProgramChunk(origData, origSize, programChunkAt, code))
		return false;
	if ((code[0] >> 16) != 1) // Vertex Shader only
		return false;

	unsigned posReg = ~0u;
	unsigned origTemps = 0;
	size_t tempsDeclAt = 0;
	size_t perViewCbDeclAt = 0;
	size_t lastCbDeclEnd = 0;
	size_t declEnd = 0;

	// Pass 1: inspect declarations.
	for (size_t at = 2; at < code.size();) {
		const unsigned op = code[at] & 2047u;
		size_t count = (code[at] >> 24) & 127u;
		if (op == kOpCustomData) {
			if (at + 1 >= code.size()) return false;
			count = code[at + 1];
		}
		if (count == 0 || count > code.size() - at)
			return false;
		const size_t end = at + count;

		if (op == kOpDclConstantBuffer) {
			if (count != 4) return false;
			const unsigned slot = code[at + 2];
			if (slot == kWarpConstantSlot)
				return false; // b13 already occupied by this shader
			if (slot == perViewSlot)
				perViewCbDeclAt = at;
			lastCbDeclEnd = end;
		} else if (op == kOpDclOutputSiv) {
			if (count >= 4 && code[end - 1] == kSbNamePosition) {
				size_t c = at + 1;
				Operand o;
				if (!Relight::IrradianceDxbc::ReadOperand(code, c, end - 1, o))
					return false;
				if (o.type == 2 && o.index != ~0u)
					posReg = o.index;
			}
		} else if (op == kOpDclTemps) {
			if (count != 2) return false;
			tempsDeclAt = at;
			origTemps = code[at + 1];
		} else if (!IsDeclarationOpcode(op) && op != kOpCustomData) {
			declEnd = at;
			break;
		}
		at = end;
	}

	if (posReg == ~0u || perViewCbDeclAt == 0 || declEnd == 0)
		return false;
	if (origTemps + 1u + snippet.tempCount > 4096u)
		return false;

	const unsigned savedPosTemp = origTemps;
	const unsigned snippetTempBase = origTemps + 1u;
	const unsigned newTotalTemps = origTemps + 1u + snippet.tempCount;

	// Ensure PerViewConstantBuffer_t declaration covers at least c0..c7 (worldToProj + projToWorld).
	if (code[perViewCbDeclAt + 3] < 8u)
		code[perViewCbDeclAt + 3] = 8u;

	// Update dcl_temps in place if it exists.
	if (tempsDeclAt != 0)
		code[tempsDeclAt + 1] = newTotalTemps;

	// Relocate snippet for this shader's registers & perViewSlot.
	Words relocated;
	if (!RelocateSnippetBody(snippet, savedPosTemp, snippetTempBase, posReg, perViewSlot, relocated))
		return false;

	// Pass 2: redirect every write to o[posReg] in the body into r[savedPosTemp], and collect `ret` sites.
	std::vector<size_t> retPositions;
	for (size_t at = declEnd; at < code.size();) {
		const unsigned op = code[at] & 2047u;
		size_t count = (code[at] >> 24) & 127u;
		if (op == kOpCustomData) {
			if (at + 1 >= code.size()) return false;
			count = code[at + 1];
			if (count == 0 || count > code.size() - at) return false;
			at += count;
			continue;
		}
		if (count == 0 || count > code.size() - at)
			return false;
		const size_t end = at + count;
		if (op == kOpRet) {
			retPositions.push_back(at);
			at = end;
			continue;
		}
		size_t cursor = at;
		uint32_t ext = code[cursor];
		while (ext & 0x80000000u) {
			if (++cursor >= end) return false;
			ext = code[cursor];
		}
		++cursor;
		while (cursor < end) {
			Operand o;
			if (!Relight::IrradianceDxbc::ReadOperand(code, cursor, end, o))
				return false;
			if (o.type == 2 && o.index == posReg) {
				const size_t idxWord = OperandIndexWord(code, o);
				if (!idxWord) return false;
				code[o.start] = (code[o.start] & ~(0xFFu << 12)) | (0u << 12); // OUTPUT -> TEMP
				code[idxWord] = savedPosTemp;
			}
		}
		at = end;
	}
	if (retPositions.empty())
		return false;

	// Splice relocated snippet before every `ret` (from back to front so earlier indices stay valid).
	for (auto it = retPositions.rbegin(); it != retPositions.rend(); ++it) {
		code.insert(code.begin() + *it, relocated.begin(), relocated.end());
	}

	// Insert `mov r[savedPosTemp].xyzw, l(0,0,0,1)` at body start so partial writes never leave garbage.
	{
		Words initPos;
		Relight::IrradianceDxbc::Emit(initPos, 54u, {
			Relight::IrradianceDxbc::Dest(savedPosTemp, 15u),
			Relight::IrradianceDxbc::Literal(0.0f, 0.0f, 0.0f, 1.0f)
		});
		code.insert(code.begin() + declEnd, initPos.begin(), initPos.end());
	}

	// Insert dcl_constantbuffer cb13[7], immediateIndexed (and dcl_temps if original lacked one).
	const size_t cbInsertAt = lastCbDeclEnd ? lastCbDeclEnd : 2;
	Words extraDecls = { 0x04000059u, 0x00208e46u, kWarpConstantSlot, kWarpConstantRows };
	if (tempsDeclAt == 0) {
		extraDecls.push_back(0x02000068u); // dcl_temps (op 104, len 2)
		extraDecls.push_back(newTotalTemps);
	}
	code.insert(code.begin() + cbInsertAt, extraDecls.begin(), extraDecls.end());

	// Update program length word.
	code[1] = static_cast<uint32_t>(code.size());

	// Rebuild DXBC container & re-sign retail MD5 checksum.
	const auto* p = static_cast<const uint8_t*>(origData);
	const unsigned chunks = ReadU32(p, 28);
	outDxbc.assign(p, p + 32 + chunks * 4);
	auto put = [&](size_t at, uint32_t value) { std::memcpy(outDxbc.data() + at, &value, 4); };
	for (unsigned i = 0; i < chunks; ++i) {
		const size_t at = ReadU32(p, 32 + i * 4);
		const size_t dst = outDxbc.size();
		put(32 + i * 4, static_cast<uint32_t>(dst));
		if (at == programChunkAt) {
			outDxbc.insert(outDxbc.end(), p + at, p + at + 8);
			outDxbc.resize(outDxbc.size() + code.size() * 4);
			std::memcpy(outDxbc.data() + dst + 8, code.data(), code.size() * 4);
			put(dst + 4, static_cast<uint32_t>(code.size() * 4));
		} else {
			const uint32_t len = ReadU32(p, at + 4);
			outDxbc.insert(outDxbc.end(), p + at, p + at + 8 + len);
		}
	}
	put(24, static_cast<uint32_t>(outDxbc.size()));
	Environment::DxbcHash::ComputeHashRetail(
		outDxbc.data() + 20, static_cast<uint32_t>(outDxbc.size() - 20), outDxbc.data() + 4);
	return true;
}

} // namespace Filmmaker::WorldWarpDxbc
