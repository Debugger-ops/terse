// parse.cpp — turns preprocessed lines into a Prompt (a list of statements).
//
// Grammar (one statement per line):
//   statement := field | include | exclude | rule | step
//   field     := KEY ':' value            e.g.  task: summarize article
//   include   := '+' ' ' value            e.g.  + type hints
//   exclude   := '-' ' ' value            e.g.  - praise
//   rule      := '!' ' ' value            e.g.  ! no invented facts
//   step      := DIGITS '.' ' ' value     e.g.  2. count missing values
//   value     := text [ ``` code fence spanning following lines ``` ]
#include "tersec.hpp"

namespace terse {

namespace {
constexpr bool ends_marker(char c) { return c == ' ' || c == '\t' || c == '\0'; }
} // namespace

Prompt parse(const PLines &lines, std::string_view file, Diagnostics &diag) {
    Prompt pr;
    pr.file = file;
    pr.nodes.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); i++) {
        const PLine &L = lines[i];
        if (L.raw) continue; // orphan fence content; already reported by the preprocessor
        const std::string_view s = L.text;
        std::size_t p = 0;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p == s.size()) continue;

        Node n;
        n.loc = L.loc;
        n.subst = L.subst;
        n.loc.col = int(p) + 1;
        n.loc.len = 1;
        std::optional<std::size_t> v;

        if (ascii::is_digit(s[p])) {
            std::size_t q = p;
            while (q < s.size() && ascii::is_digit(s[q])) q++;
            if (at(s, q) == '.' && ends_marker(at(s, q + 1))) {
                n.kind = NodeKind::Step;
                n.step = to_int(s.substr(p, q - p));
                n.loc.len = int(q - p + 1);
                v = q + 1;
            }
        }
        if (!v && (s[p] == '+' || s[p] == '-' || s[p] == '!') && ends_marker(at(s, p + 1))) {
            n.kind = s[p] == '+' ? NodeKind::Plus : s[p] == '-' ? NodeKind::Minus : NodeKind::Bang;
            v = p + 1;
        }
        if (!v && (ascii::is_alpha(s[p]) || s[p] == '_')) {
            std::size_t q = p;
            while (q < s.size() && (ascii::is_alnum(s[q]) || s[q] == '_' || s[q] == '-')) q++;
            if (at(s, q) == ':') {
                n.kind = NodeKind::Field;
                n.key = std::string(s.substr(p, q - p));
                n.loc.len = int(q - p + 1);
                v = q + 1;
            }
        }
        if (!v) {
            Loc l = n.loc;
            std::size_t e = p;
            while (e < s.size() && s[e] != ' ') e++;
            l.len = int(e - p);
            diag.error(l, "expected 'key:', '+', '-', '!' or a step number like '1.' at start of line");
            continue;
        }
        std::size_t vi = *v;
        while (vi < s.size() && (s[vi] == ' ' || s[vi] == '\t')) vi++;
        n.vcol = int(vi) + 1;

        std::string_view val = s.substr(vi);
        while (!val.empty() && ascii::is_space(val.back())) val.remove_suffix(1);
        std::string value(val);

        // a value that opens a ``` fence swallows the raw lines that follow
        if (count_substr(value, "```") % 2 == 1) {
            while (i + 1 < lines.size() && lines[i + 1].raw) {
                i++;
                value += '\n';
                value += lines[i].text;
                if (count_substr(lines[i].text, "```") % 2 == 1) break;
            }
        }
        n.orig_value = value;
        n.value = std::move(value);
        pr.nodes.push_back(std::move(n));
    }
    return pr;
}

} // namespace terse
