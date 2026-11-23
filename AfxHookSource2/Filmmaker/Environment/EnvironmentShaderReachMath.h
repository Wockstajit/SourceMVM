#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Filmmaker::Environment {
// WHAT ONE ENTRY POINT COMPILES. The Environment programs share one large HLSL source. Every
// program used to be compiled from, and its cache key used to hash, the whole of it, so an edit to
// the fog march rebuilt the rain and the mesh programs as well. A program is now compiled from the
// declarations its entry point reaches and nothing else, and its key hashes that text: an edit
// rebuilds only the programs that use what changed.
//
// The source is cut into top-level items: functions, structs, typedefs, static constants, macro
// definitions, the uniform declarations (resources, samplers, cbuffers) and the conditional
// directives. An item is kept when the entry point reaches it by name, through any chain of uses.
// Uniform declarations and conditional directives are kept for every entry, so register layouts and
// #if structure stay exactly as written. Comments are dropped, so editing one rebuilds nothing.
// Same-named items (overloads, #if alternatives) are kept together.
//
// When the layout is anything this parser does not understand (a directive inside a declaration,
// an #if branch that opens a brace it does not close, unbalanced brackets, an unknown entry), the
// whole source is used: slower to compile, never different.
struct ShaderReachItem {
    size_t begin = 0, end = 0;        // the item's text in ShaderReachSource::clean
    std::vector<std::string> names;   // what the item declares
    std::vector<std::string> uses;    // identifiers it mentions (member names after '.' excluded)
    bool always = false;              // kept for every entry
};
struct ShaderReachSource {
    std::string clean; // the source with comments replaced as the preprocessor would
    std::vector<ShaderReachItem> items;
    std::unordered_map<std::string, std::vector<size_t>> declaredBy;
    bool understood = false;
};

namespace ShaderReachDetail {
inline bool IdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool IdentChar(char c) { return IdentStart(c) || (c >= '0' && c <= '9'); }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
inline bool Blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
inline bool Space(char c) { return Blank(c) || c == '\n'; }

struct Token {
    std::string_view text;
    int depth; // bracket depth outside the token; an opener or closer carries its outer depth
    bool ident;
};
// Identifiers, numbers (one token, suffixes and exponents included), strings and single characters.
inline std::vector<Token> Tokenize(std::string_view s) {
    std::vector<Token> tokens;
    int depth = 0;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (Space(c) || c == '\\') { ++i; continue; }
        const size_t start = i;
        if (IdentStart(c)) {
            while (i < s.size() && IdentChar(s[i])) ++i;
            tokens.push_back({s.substr(start, i - start), depth, true});
        } else if (Digit(c) || (c == '.' && i + 1 < s.size() && Digit(s[i + 1]))) {
            while (i < s.size() && (IdentChar(s[i]) || s[i] == '.' ||
                                    ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                ++i;
            tokens.push_back({s.substr(start, i - start), depth, false});
        } else if (c == '"' || c == '\'') {
            for (++i; i < s.size() && s[i] != c; ++i)
                if (s[i] == '\\') ++i;
            i = i < s.size() ? i + 1 : i;
            tokens.push_back({s.substr(start, i - start), depth, false});
        } else {
            ++i;
            if (c == ')' || c == ']' || c == '}') --depth;
            tokens.push_back({s.substr(start, 1), depth, false});
            if (c == '(' || c == '[' || c == '{') ++depth;
        }
    }
    return tokens;
}
inline void AddUses(std::vector<std::string> &uses, const std::vector<Token> &tokens, size_t from = 0) {
    for (size_t k = from; k < tokens.size(); ++k)
        if (tokens[k].ident && !(k > 0 && tokens[k - 1].text == "."))
            uses.emplace_back(tokens[k].text);
}
// Declarator names of `type a [n] : register(x) = init, b ;` at the outermost depth.
inline void AddDeclarators(std::vector<std::string> &names, const std::vector<Token> &tokens, size_t from) {
    bool init = false, semantic = false;
    for (size_t k = from; k < tokens.size(); ++k) {
        const auto &t = tokens[k];
        if (t.depth != 0) continue;
        if (t.text == "=") { init = true; continue; }
        if (t.text == ",") { init = semantic = false; continue; }
        if (t.text == ":" && !init) { semantic = true; continue; }
        if (!t.ident || init || semantic || k == from || k + 1 >= tokens.size()) continue;
        const auto next = tokens[k + 1].text;
        if (next == "=" || next == ";" || next == "," || next == "[" || next == ":")
            names.emplace_back(t.text);
    }
}
// Directive keyword and the rest of its line.
inline void SplitDirective(std::string_view line, std::string_view &keyword, std::string_view &rest) {
    size_t i = line.find('#') + 1;
    while (i < line.size() && Blank(line[i])) ++i;
    size_t start = i;
    while (i < line.size() && IdentChar(line[i])) ++i;
    keyword = line.substr(start, i - start);
    rest = line.substr(i);
}
} // namespace ShaderReachDetail

inline ShaderReachSource ParseShaderReach(std::string_view source) {
    using namespace ShaderReachDetail;
    ShaderReachSource out;
    // Comments go the way the preprocessor removes them: a block comment becomes one space.
    std::string &clean = out.clean;
    clean.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (c == '"') {
            const size_t start = i;
            for (++i; i < source.size() && source[i] != '"' && source[i] != '\n'; ++i)
                if (source[i] == '\\') ++i;
            i = i < source.size() ? i + 1 : i;
            clean.append(source.substr(start, i - start));
        } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const size_t end = source.find("*/", i + 2);
            if (end == std::string_view::npos) return out;
            i = end + 2;
            clean += ' ';
        } else {
            clean += c;
            ++i;
        }
    }
    const std::string_view s = out.clean;
    struct Pending {
        size_t start;
        bool equals = false; // an '=' before the first brace: an initializer, not a body
        std::string_view first;
        int braceEnds = -1; // 1: the item ends at its closing brace; 0: at the next ';'
        std::vector<std::string> bodyMacros, bodyMacroUses;
    };
    std::vector<int> conditionDepths;
    bool active = false;
    Pending item{};
    int depth = 0;
    bool lineStart = true;
    auto finish = [&](size_t end) {
        const auto text = s.substr(item.start, end - item.start);
        const auto tokens = Tokenize(text);
        ShaderReachItem result;
        result.begin = item.start;
        result.end = end;
        AddUses(result.uses, tokens);
        result.uses.insert(result.uses.end(), item.bodyMacroUses.begin(), item.bodyMacroUses.end());
        result.names = item.bodyMacros;
        size_t at = 0;
        while (at < tokens.size() && tokens[at].text == "[") { // attributes
            const int open = tokens[at].depth;
            for (++at; at < tokens.size() && !(tokens[at].text == "]" && tokens[at].depth == open); ++at) {}
            ++at;
        }
        bool isStatic = false;
        for (size_t k = at; k < tokens.size() && tokens[k].depth == 0; ++k)
            if (tokens[k].text == "static") isStatic = true;
        const std::string_view first = at < tokens.size() ? tokens[at].text : std::string_view{};
        if (first == "struct") {
            if (at + 1 < tokens.size() && tokens[at + 1].ident) result.names.emplace_back(tokens[at + 1].text);
            // `struct S {...} s;` also declares a uniform.
            size_t k = at;
            while (k < tokens.size() && tokens[k].text != "{") ++k;
            for (; k < tokens.size(); ++k)
                if (tokens[k].depth == 0 && tokens[k].ident) result.always = true;
        } else if (first == "typedef") {
            std::string_view last;
            for (size_t k = at; k < tokens.size(); ++k)
                if (tokens[k].depth == 0 && tokens[k].ident) last = tokens[k].text;
            if (!last.empty()) result.names.emplace_back(last);
        } else if (first == "cbuffer" || first == "tbuffer") {
            result.always = true;
        } else {
            // A function (definition or prototype): the outermost '(' not after ':' or '='.
            size_t open = tokens.size();
            for (size_t k = at; k < tokens.size() && tokens[k].text != "{"; ++k) {
                if (tokens[k].depth != 0) continue;
                if (tokens[k].text == ":" || tokens[k].text == "=") break;
                if (tokens[k].text == "(") { open = k; break; }
            }
            if (open < tokens.size() && open > at && tokens[open - 1].ident)
                result.names.emplace_back(tokens[open - 1].text);
            else if (isStatic)
                AddDeclarators(result.names, tokens, at);
            else
                result.always = true; // a uniform: resource, sampler, global constant
        }
        if (result.names.empty()) result.always = true;
        out.items.push_back(std::move(result));
        active = false;
    };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\n') { lineStart = true; ++i; continue; }
        if (Blank(c)) { ++i; continue; }
        if (c == '#' && lineStart) {
            // A directive runs to the end of its line, with backslash continuations.
            size_t end = i;
            while (end < s.size() && s[end] != '\n') {
                if (s[end] == '\\' && end + 1 < s.size() && s[end + 1] == '\n') ++end;
                ++end;
            }
            const auto line = s.substr(i, end - i);
            std::string_view keyword, rest;
            SplitDirective(line, keyword, rest);
            if (keyword == "if" || keyword == "ifdef" || keyword == "ifndef") conditionDepths.push_back(depth);
            else if (keyword == "elif" || keyword == "else" || keyword == "endif") {
                // Every branch must leave the brackets as it found them.
                if (conditionDepths.empty() || conditionDepths.back() != depth) return out;
                if (keyword == "endif") conditionDepths.pop_back();
            }
            if (active) {
                // Inside a body a directive is part of the item; a macro it defines is the item's.
                if (depth == 0) return out;
                if (keyword == "define") {
                    const auto tokens = Tokenize(rest);
                    if (tokens.empty() || !tokens[0].ident) return out;
                    item.bodyMacros.emplace_back(tokens[0].text);
                    AddUses(item.bodyMacroUses, tokens, 1);
                }
            } else {
                ShaderReachItem directive;
                directive.begin = i;
                directive.end = end;
                const auto tokens = Tokenize(rest);
                if (keyword == "define" && !tokens.empty() && tokens[0].ident) {
                    directive.names.emplace_back(tokens[0].text);
                    AddUses(directive.uses, tokens, 1);
                } else {
                    directive.always = true;
                    AddUses(directive.uses, tokens);
                }
                out.items.push_back(std::move(directive));
            }
            i = end;
            continue;
        }
        lineStart = false;
        if (!active) {
            if (c == ';') { ++i; continue; } // a stray empty declaration
            item = Pending{i};
            active = true;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < s.size() && s[i] != c && s[i] != '\n'; ++i)
                if (s[i] == '\\') ++i;
            ++i;
            continue;
        }
        if (IdentStart(c)) {
            const size_t start = i;
            while (i < s.size() && IdentChar(s[i])) ++i;
            if (depth == 0 && item.first.empty()) item.first = s.substr(start, i - start);
            continue;
        }
        ++i;
        if (depth == 0) {
            if (c == '=') item.equals = true;
            if (c == ';' && item.braceEnds != 1) { finish(i); continue; }
            if (c == '{' && item.braceEnds < 0)
                item.braceEnds = item.first == "struct" || item.first == "typedef" || item.equals ? 0 : 1;
        }
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') {
            if (--depth < 0) return out;
            if (depth == 0 && c == '}' && item.braceEnds == 1) {
                size_t end = i;
                while (end < s.size() && Blank(s[end])) ++end;
                if (end < s.size() && s[end] == ';') i = end + 1;
                finish(i);
            }
        }
    }
    if (active || depth != 0 || !conditionDepths.empty()) return out;
    for (size_t k = 0; k < out.items.size(); ++k)
        for (const auto &name : out.items[k].names) out.declaredBy[name].push_back(k);
    out.understood = true;
    return out;
}

// The text `entry` compiles from: the items it reaches, in source order, one per line. Empty when
// the source was not understood or nothing declares `entry`; callers then use the whole source.
inline std::string ShaderReachText(const ShaderReachSource &parsed, std::string_view entry) {
    if (!parsed.understood || !parsed.declaredBy.count(std::string(entry))) return {};
    std::vector<char> keep(parsed.items.size(), 0);
    std::vector<std::string_view> work{entry};
    for (size_t k = 0; k < parsed.items.size(); ++k)
        if (parsed.items[k].always) {
            keep[k] = 1;
            for (const auto &use : parsed.items[k].uses) work.push_back(use);
        }
    std::unordered_set<std::string_view> seen;
    while (!work.empty()) {
        const auto name = work.back();
        work.pop_back();
        if (!seen.insert(name).second) continue;
        const auto found = parsed.declaredBy.find(std::string(name));
        if (found == parsed.declaredBy.end()) continue;
        for (const size_t k : found->second)
            if (!keep[k]) {
                keep[k] = 1;
                for (const auto &use : parsed.items[k].uses) work.push_back(use);
            }
    }
    std::string text;
    for (size_t k = 0; k < parsed.items.size(); ++k)
        if (keep[k]) {
            text.append(parsed.clean, parsed.items[k].begin, parsed.items[k].end - parsed.items[k].begin);
            text += '\n';
        }
    return text;
}
} // namespace Filmmaker::Environment
