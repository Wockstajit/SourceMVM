#pragma once

// Engine-free half of the bug report (Community/BugReport.cpp): the text rules that keep a report
// readable and small, the always-on line recorder, and the Discord upload body. Tested by
// Automation/tests/bug-report-math-tests.cpp.
//
// The report is ONE plain .txt file, not a zip: Discord shows a text file inline and does not flag
// it, while archives get scanned, warned about or stripped. It goes to the developer's private
// channel through a webhook as an attachment (Discord's limit there is far above kMaxReportBytes).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace Filmmaker::BugReport {

constexpr size_t kMaxReportBytes = 2 * 1024 * 1024; // the whole file
constexpr size_t kRecorderBytes = 768 * 1024;       // the always-on recent history kept in memory
constexpr size_t kDebugLogBudget = 700 * 1024;      // newest mvm_debug log, its last part
constexpr size_t kConsoleLogBudget = 200 * 1024;    // CS2's console.log (-condebug), its last part
constexpr size_t kMaxDescription = 4000;            // what the user typed
constexpr size_t kMaxLineBytes = 1000;              // one recorded line, longer ones are cut

// Every Windows user folder in `text` becomes %USERPROFILE%, so a report never carries the
// account name: "C:\Users\ExampleUser\AppData\..." -> "%USERPROFILE%\AppData\...". `profileDir` is
// this machine's %USERPROFILE% (either slash style is matched, case-insensitively); any other
// "<drive>:\Users\<name>" is replaced as well, e.g. a path logged under another account.
// Public, Default and All Users are left alone: they are not a person.
inline std::string ScrubUserPaths(const std::string& text, const std::string& profileDir) {
	auto lower = [](std::string s) {
		for (char& c : s) c = (char)std::tolower((unsigned char)c);
		return s;
	};
	auto endsName = [](char c) {
		return c == '\\' || c == '/' || c == '"' || c == '\'' || c == '\n' || c == '\r' || c == '\t'
			|| c == ')' || c == ';' || c == ',' || c == '\0';
	};
	const std::string kToken = "%USERPROFILE%";
	std::string out = text;

	// 1. This machine's own profile folder, but only as a whole path segment: "C:\Users\Example" must
	//    not eat the start of "C:\Users\ExampleUser2".
	if (profileDir.size() > 3) {
		for (int style = 0; style < 2; ++style) {
			std::string needle = profileDir;
			if (style == 1) std::replace(needle.begin(), needle.end(), '\\', '/');
			const std::string lowNeedle = lower(needle);
			const std::string low = lower(out);
			std::string result;
			size_t last = 0, at;
			while ((at = low.find(lowNeedle, last)) != std::string::npos) {
				const size_t end = at + lowNeedle.size();
				const char next = end < out.size() ? out[end] : '\0';
				result.append(out, last, at - last);
				result += endsName(next) || next == ' ' ? kToken : out.substr(at, end - at);
				last = end;
			}
			result.append(out, last, std::string::npos);
			out = result;
		}
	}

	// 2. Any "<drive>:\Users\<name>" left (another account, either slash style).
	std::string result;
	size_t i = 0;
	while (i < out.size()) {
		const bool drive = i + 9 <= out.size() && std::isalpha((unsigned char)out[i]) && out[i + 1] == ':'
			&& (out[i + 2] == '\\' || out[i + 2] == '/') && lower(out.substr(i + 3, 5)) == "users"
			&& (out[i + 8] == '\\' || out[i + 8] == '/');
		if (!drive) { result += out[i++]; continue; }
		size_t end = i + 9;
		while (end < out.size() && !endsName(out[end])) ++end;
		const std::string name = lower(out.substr(i + 9, end - (i + 9)));
		if (name.empty() || name == "public" || name == "default" || name == "all users") {
			result.append(out, i, end - i);
		} else {
			result += kToken;
		}
		i = end;
	}
	return result;
}

// Steam identity in recorded text becomes a placeholder, so a report never carries who played. CS2's
// own console lines name the local player ("<name> connected") and log their SteamID. Replaced:
//  - SteamID64: a 17-digit run starting 7656119, not part of a longer number -> <steamid>
//  - SteamID3 "[U:1:<n>]" -> [U:1:<steamid>], SteamID2 "STEAM_<x>:<y>:<n>" -> STEAM_<steamid>
//  - `playerName` (the local Steam name, exact match) -> <player>. Names under 3 bytes are left
//    alone: replacing a one- or two-letter name would mangle ordinary words.
inline std::string ScrubSteamIdentity(const std::string& text, const std::string& playerName) {
	auto digit = [](char c) { return c >= '0' && c <= '9'; };
	std::string out;
	out.reserve(text.size());
	size_t i = 0;
	while (i < text.size()) {
		// SteamID64
		if (digit(text[i]) && (i == 0 || !digit(text[i - 1])) && text.compare(i, 7, "7656119") == 0) {
			size_t end = i;
			while (end < text.size() && digit(text[end])) ++end;
			if (end - i == 17) { out += "<steamid>"; i = end; continue; }
		}
		// SteamID3
		if (text.compare(i, 5, "[U:1:") == 0) {
			size_t end = i + 5;
			while (end < text.size() && digit(text[end])) ++end;
			if (end > i + 5 && end < text.size() && text[end] == ']') { out += "[U:1:<steamid>]"; i = end + 1; continue; }
		}
		// SteamID2
		if (text.compare(i, 6, "STEAM_") == 0 && i + 11 <= text.size() && digit(text[i + 6]) && text[i + 7] == ':'
			&& digit(text[i + 8]) && text[i + 9] == ':' && digit(text[i + 10])) {
			size_t end = i + 10;
			while (end < text.size() && digit(text[end])) ++end;
			out += "STEAM_<steamid>";
			i = end;
			continue;
		}
		out += text[i++];
	}
	if (playerName.size() < 3) return out;
	std::string result;
	size_t last = 0, at;
	while ((at = out.find(playerName, last)) != std::string::npos) {
		result.append(out, last, at - last);
		result += "<player>";
		last = at + playerName.size();
	}
	result.append(out, last, std::string::npos);
	return result;
}

// The last `maxBytes` of `text`, starting on a whole line and never inside a UTF-8 sequence.
// `omitted` receives how many bytes were cut from the front (0 when it all fit).
inline std::string TailText(const std::string& text, size_t maxBytes, size_t* omitted = nullptr) {
	if (omitted) *omitted = 0;
	if (text.size() <= maxBytes) return text;
	size_t start = text.size() - maxBytes;
	const size_t newline = text.find('\n', start);
	// Start after the next line break when there is one within reach; otherwise just skip any
	// UTF-8 continuation bytes so the tail begins on a whole character.
	if (newline != std::string::npos && newline + 1 < text.size()) start = newline + 1;
	else while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
	if (omitted) *omitted = start;
	return text.substr(start);
}

// What the user typed, made safe to embed: control characters dropped (line breaks and tabs kept,
// CR LF folded to LF), surrounding blank space trimmed, at most kMaxDescription bytes on a whole
// UTF-8 character.
inline std::string CleanDescription(const std::string& raw) {
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(raw[i]);
		if (c == '\r') { if (i + 1 < raw.size() && raw[i + 1] == '\n') continue; out += '\n'; continue; }
		if (c < 0x20 && c != '\n' && c != '\t') continue;
		if (c == 0x7F) continue;
		out += static_cast<char>(c);
	}
	const size_t first = out.find_first_not_of(" \t\n");
	if (first == std::string::npos) return {};
	const size_t last = out.find_last_not_of(" \t\n");
	out = out.substr(first, last - first + 1);
	if (out.size() > kMaxDescription) {
		size_t cut = kMaxDescription;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
		out.resize(cut);
	}
	return out;
}

// The JS side sends the description through encodeURIComponent (the command channel is
// line-based, and a description has line breaks). Malformed escapes are kept as typed.
inline std::string DecodeUriComponent(const std::string& in) {
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '%' && i + 2 < in.size()) {
			const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
			if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); i += 2; continue; }
		}
		out += in[i];
	}
	return out;
}

// "SourceMVM-bug-2026-10-10_01-40-12.txt": sorts by time, and says what it is in a Discord DM.
inline std::string ReportFileName(int year, int month, int day, int hour, int minute, int second) {
	char name[64];
	std::snprintf(name, sizeof(name), "SourceMVM-bug-%04d-%02d-%02d_%02d-%02d-%02d.txt", year, month, day, hour, minute, second);
	return name;
}

// The always-on flight recorder: the most recent lines, at most `capacity` bytes in all. Oldest lines
// fall off the front, so a report always holds what happened just before it. Not thread-safe; the
// owner locks around it.
class LineRing {
public:
	explicit LineRing(size_t capacity) : capacity_(capacity) {}
	void Add(std::string line) {
		if (line.size() > kMaxLineBytes) {
			size_t cut = kMaxLineBytes;
			while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
			line.resize(cut);
			line += " [...]";
		}
		bytes_ += line.size() + 1;
		lines_.push_back(std::move(line));
		++total_;
		while (bytes_ > capacity_ && !lines_.empty()) { bytes_ -= lines_.front().size() + 1; lines_.pop_front(); }
	}
	std::string Text() const {
		std::string out;
		out.reserve(bytes_);
		for (const auto& l : lines_) { out += l; out += '\n'; }
		return out;
	}
	size_t Lines() const { return lines_.size(); }
	size_t Dropped() const { return size_t(total_ - lines_.size()); }
	size_t Bytes() const { return bytes_; }
private:
	size_t capacity_;
	size_t bytes_ = 0;
	unsigned long long total_ = 0;
	std::deque<std::string> lines_;
};

// One file of an upload.
struct UploadFile {
	std::string name;
	std::string bytes;
	std::string contentType = "text/plain; charset=utf-8";
};

// The body of a Discord webhook upload: multipart/form-data with the message JSON as
// "payload_json" and the files as "files[0]", "files[1]", ... `boundary` must not occur in any part.
inline std::string MultipartBody(const std::string& boundary, const std::string& payloadJson,
	const std::vector<UploadFile>& files) {
	size_t total = payloadJson.size() + 512;
	for (const UploadFile& f : files) total += f.bytes.size() + 256;
	std::string b;
	b.reserve(total);
	b += "--" + boundary + "\r\n";
	b += "Content-Disposition: form-data; name=\"payload_json\"\r\n";
	b += "Content-Type: application/json\r\n\r\n";
	b += payloadJson + "\r\n";
	for (size_t i = 0; i < files.size(); ++i) {
		b += "--" + boundary + "\r\n";
		b += "Content-Disposition: form-data; name=\"files[" + std::to_string(i) + "]\"; filename=\"" + files[i].name + "\"\r\n";
		b += "Content-Type: " + files[i].contentType + "\r\n\r\n";
		b += files[i].bytes + "\r\n";
	}
	b += "--" + boundary + "--\r\n";
	return b;
}

// Discord refuses a webhook upload over 10 MB. A crash dump rides along only below this, leaving
// room for the report (at most 2 MB) and the form overhead.
constexpr size_t kMaxDumpUploadBytes = 7u * 1024 * 1024;

// A Discord webhook URL, so a typo or a pasted message link is refused before anything is sent.
inline bool IsDiscordWebhookUrl(const std::string& url) {
	static const char* const kPrefixes[] = {"https://discord.com/api/webhooks/", "https://discordapp.com/api/webhooks/",
		"https://canary.discord.com/api/webhooks/", "https://ptb.discord.com/api/webhooks/"};
	for (const char* p : kPrefixes) {
		const std::string prefix(p);
		if (url.compare(0, prefix.size(), prefix) != 0) continue;
		const std::string rest = url.substr(prefix.size());
		const size_t slash = rest.find('/');
		if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) return false;
		for (size_t i = 0; i < slash; ++i) if (rest[i] < '0' || rest[i] > '9') return false;
		for (size_t i = slash + 1; i < rest.size(); ++i) {
			const char c = rest[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false;
		}
		return true;
	}
	return false;
}

} // namespace Filmmaker::BugReport
