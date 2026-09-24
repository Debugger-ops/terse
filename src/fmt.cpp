// fmt.cpp — `terse fmt`: canonical layout for Terse source.
//
//   task:summarize    ->  task: summarize          one space after every marker
//     +   type hints  ->  + type hints             statements start at the block's indent
//   #  define X  1    ->  #define X  1             '#name', then the rest of the line as written
//   (3 blank lines)   ->  (1 blank line)           no leading/trailing blank lines
//   #if / #template / #for bodies                  indented 2 spaces per level
//
// Values, comments, block comments and ``` fences are never changed (only trailing blanks go).
#include "tersec.hpp"

#include <algorithm>

namespace terse {

namespace {

std::string rstrip(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

std::size_t skip_blanks(std::string_view s, std::size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

// Length of the statement marker at the start of s ("task:", "+", "12."), or 0.
std::size_t marker_len(std::string_view s) {
    if (s.empty()) return 0;
    auto ends = [&](std::size_t q) { return q >= s.size() || s[q] == ' ' || s[q] == '\t'; };
    if (ascii::is_digit(s[0])) {
        std::size_t q = 0;
        while (q < s.size() && ascii::is_digit(s[q])) q++;
        if (at(s, q) == '.' && ends(q + 1)) return q + 1;
    }
    if ((s[0] == '+' || s[0] == '-' || s[0] == '!') && ends(1)) return 1;
    if (ascii::is_alpha(s[0]) || s[0] == '_') {
        std::size_t q = 0;
        while (q < s.size() && (ascii::is_alnum(s[q]) || s[q] == '_' || s[q] == '-')) q++;
        if (at(s, q) == ':') return q + 1;
    }
    return 0;
}

} // namespace

std::string format_source(std::string_view raw) {
    SourceText src = split_source(raw);
    const std::string eol = !src.eols.empty() && src.eols[0] == "\r\n" ? "\r\n" : "\n";

    std::vector<std::string> out;
    bool in_block = false, in_fence = false, pending_blank = false;
    int level = 0;

    auto emit = [&](std::string line) {
        if (pending_blank && !out.empty()) out.emplace_back();
        pending_blank = false;
        out.push_back(std::move(line));
    };

    for (const std::string &line : src.lines) {
        if (in_fence) {                                   // fence content: byte-for-byte
            out.push_back(line);
            if (count_substr(line, "```") % 2 == 1) in_fence = false;
            continue;
        }
        const bool started_in_block = in_block;
        const std::string code = strip_comments(line, in_block);
        const std::string_view t = std::string_view(line).substr(skip_blanks(line, 0));

        if (t.empty()) { pending_blank = true; continue; }
        if (started_in_block) { emit(rstrip(line)); continue; }       // inside /* ... */

        const std::string pad(std::size_t(2 * level), ' ');
        const std::string_view c = std::string_view(code).substr(skip_blanks(code, 0));

        if (c.empty()) { emit(pad + rstrip(t)); continue; }            // comment-only line

        if (c[0] == '#') {                                             // directive
            std::size_t q = skip_blanks(t, 1), n = q;
            while (n < t.size() && is_ident_char(t[n])) n++;
            const std::string name(t.substr(q, n - q));
            const std::string rest = rstrip(t.substr(skip_blanks(t, n)));
            int here = level;
            if (name == "end" || name == "endif") here = level = std::max(0, level - 1);
            else if (name == "else" || name == "elif") here = std::max(0, level - 1);
            emit(cat(std::string(std::size_t(2 * here), ' '), '#', name, rest.empty() ? "" : " ", rest));
            if (name == "if" || name == "ifdef" || name == "ifndef" || name == "template" || name == "for") level++;
            continue;
        }

        const std::size_t m = marker_len(t);
        if (m == 0) { emit(pad + rstrip(t)); continue; }               // not a statement: leave it
        const std::string value = rstrip(t.substr(skip_blanks(t, m)));
        emit(cat(pad, t.substr(0, m), value.empty() ? "" : " ", value));
        if (count_substr(code, "```") % 2 == 1) in_fence = true;
    }

    std::string result = src.bom;
    for (const auto &l : out) result += l + eol;
    return result;
}

} // namespace terse
