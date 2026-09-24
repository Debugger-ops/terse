// sema.cpp — semantic checks, lint warnings and the optimizer.
//
// Errors (the prompt is rejected, like a C compile error):
//   unknown key, empty value, redefinition of task:/out:, malformed let:,
//   'if:' without '->', steps out of sequence, reference to a missing step,
//   prompt with neither task: nor steps.
//
// Warnings (-W<name>, all fixable by the optimizer where marked):
//   filler*, unicode*, numeronym*, abbrev*, number-words*, duplicate*,
//   missing-out, unused-let, err-quote, repeat-key, redefined, user, unknown-pragma;
//   with -Wextra: field-order, articles* (fixed at -O2), out-length.
#include "tersec.hpp"

#include <algorithm>

namespace terse {

namespace {

// ---------------------------------------------------------------- keys

struct KeyInfo {
    std::string_view name;
    int rank;     // canonical order
    int single;   // 2 = error if repeated, 1 = warning if repeated
};

constexpr KeyInfo kKeys[] = {
    {"task", 0, 2}, {"let", 1, 0},  {"ctx", 2, 0}, {"in", 2, 0},  {"err", 2, 0},
    {"for", 2, 1},  {"tone", 2, 1}, {"ask", 2, 0}, {"why", 2, 0}, {"ref", 2, 0},
    {"if", 2, 0},   {"out", 5, 2},
};

const KeyInfo *key_info(std::string_view k) {
    for (const auto &ki : kKeys)
        if (ki.name == k) return &ki;
    return nullptr;
}

// ------------------------------------------ protected text: "quotes", `code`, ``` fences ```

using Mask = std::vector<unsigned char>;

Mask protect_mask(std::string_view s) {
    const std::size_t n = s.size();
    Mask m(n + 1, 0);
    std::size_t i = 0;
    while (i < n) {
        std::size_t end = 0;
        if (s.compare(i, 3, "```") == 0) {
            std::size_t e = s.find("```", i + 3);
            end = e != std::string_view::npos ? e + 3 : n;
        } else if (s[i] == '`' || s[i] == '"') {
            std::size_t e = s.find(s[i], i + 1);
            end = e != std::string_view::npos ? e + 1 : n;
        }
        if (end) {
            std::fill(m.begin() + std::ptrdiff_t(i), m.begin() + std::ptrdiff_t(end), 1);
            i = end;
        } else {
            i++;
        }
    }
    return m;
}

bool unprotected_contains(std::string_view s, std::string_view needle) {
    const Mask m = protect_mask(s);
    for (std::size_t p = s.find(needle); p != std::string_view::npos; p = s.find(needle, p + 1)) {
        std::size_t j = 0;
        while (j < needle.size() && !m[p + j]) j++;
        if (j == needle.size()) return true;
    }
    return false;
}

// ---------------------------------------------------------------- lint rules

struct Rule {
    std::string_view from;
    std::string_view to;
    Warn w;
    int opt;      // optimizer level that applies the fix
};

constexpr Rule kRules[] = {
    // R1: filler (longest first)
    {"i would like you to", "", W_FILLER, 1}, {"i'd like you to", "", W_FILLER, 1},
    {"i want you to", "", W_FILLER, 1},       {"can you please", "", W_FILLER, 1},
    {"could you please", "", W_FILLER, 1},    {"can you help me", "", W_FILLER, 1},
    {"could you help me", "", W_FILLER, 1},   {"would you mind", "", W_FILLER, 1},
    {"could you", "", W_FILLER, 1},           {"can you", "", W_FILLER, 1},
    {"would you", "", W_FILLER, 1},           {"please", "", W_FILLER, 1},
    {"kindly", "", W_FILLER, 1},              {"thank you", "", W_FILLER, 1},
    {"thanks", "", W_FILLER, 1},
    // plain ASCII beats unicode symbols
    {"\xE2\x86\x92", "->", W_UNICODE, 1},  // →
    {"\xE2\x87\x92", "->", W_UNICODE, 1},  // ⇒
    {"\xE2\x89\xA4", "<=", W_UNICODE, 1},  // ≤
    {"\xE2\x89\xA5", ">=", W_UNICODE, 1},  // ≥
    {"\xE2\x89\xA0", "!=", W_UNICODE, 1},  // ≠
    {"\xE2\x80\x94", "-", W_UNICODE, 1},   // —
    {"\xE2\x80\x93", "-", W_UNICODE, 1},   // –
    {"\xE2\x80\x9C", "\"", W_UNICODE, 1},  // “
    {"\xE2\x80\x9D", "\"", W_UNICODE, 1},  // ”
    {"\xE2\x80\x98", "'", W_UNICODE, 1},   // ‘
    {"\xE2\x80\x99", "'", W_UNICODE, 1},   // ’
    {"\xE2\x80\xA6", "...", W_UNICODE, 1}, // …
    {"\xC3\x97", "x", W_UNICODE, 1},       // ×
    // R8: numeronyms cost more than the full word
    {"k8s", "kubernetes", W_NUMERONYM, 1},     {"i18n", "internationalization", W_NUMERONYM, 1},
    {"a11y", "accessibility", W_NUMERONYM, 1}, {"l10n", "localization", W_NUMERONYM, 1},
    {"o11y", "observability", W_NUMERONYM, 1},
    // §6: short forms that cost more
    {"w/o", "without", W_ABBREV, 1}, {"w/", "with", W_ABBREV, 1}, {"e.g.", "like", W_ABBREV, 1},
    // R10: digits
    {"two", "2", W_NUMBER_WORDS, 1},     {"three", "3", W_NUMBER_WORDS, 1},
    {"four", "4", W_NUMBER_WORDS, 1},    {"five", "5", W_NUMBER_WORDS, 1},
    {"six", "6", W_NUMBER_WORDS, 1},     {"seven", "7", W_NUMBER_WORDS, 1},
    {"eight", "8", W_NUMBER_WORDS, 1},   {"nine", "9", W_NUMBER_WORDS, 1},
    {"ten", "10", W_NUMBER_WORDS, 1},    {"eleven", "11", W_NUMBER_WORDS, 1},
    {"twelve", "12", W_NUMBER_WORDS, 1}, {"fifteen", "15", W_NUMBER_WORDS, 1},
    {"twenty", "20", W_NUMBER_WORDS, 1}, {"one hundred", "100", W_NUMBER_WORDS, 1},
    {"a hundred", "100", W_NUMBER_WORDS, 1}, {"hundred", "100", W_NUMBER_WORDS, 1},
    // R2: articles (aggressive, -O2)
    {"the ", "", W_ARTICLES, 2}, {"an ", "", W_ARTICLES, 2}, {"a ", "", W_ARTICLES, 2},
};

bool rule_matches(const Rule &r, std::string_view s, std::size_t i, const Mask &mask) {
    const std::size_t l = r.from.size();
    if (!istarts_with(s, i, r.from)) return false;
    for (std::size_t j = 0; j < l; j++)
        if (mask[i + j]) return false;
    if (is_word_char(r.from.front()) && i > 0 && is_word_char(s[i - 1])) return false;
    if (is_word_char(r.from.back()) && is_word_char(at(s, i + l))) return false;
    return true;
}

unsigned char byte(std::string_view s, std::size_t i) { return static_cast<unsigned char>(at(s, i)); }

// emoji and pictographs: 4-byte UTF-8, U+2600..U+27BF, plus joiners/variation selectors
std::size_t emoji_len(std::string_view s, std::size_t i) {
    unsigned char b0 = byte(s, i), b1 = byte(s, i + 1), b2 = byte(s, i + 2), b3 = byte(s, i + 3);
    if (b0 >= 0xF0 && b0 <= 0xF4 && b1 && b2 && b3) return 4;
    if (b0 == 0xE2 && b1 >= 0x98 && b1 <= 0x9E && b2) return 3;
    return 0;
}

std::size_t joiner_len(std::string_view s, std::size_t i) {
    unsigned char b0 = byte(s, i), b1 = byte(s, i + 1), b2 = byte(s, i + 2);
    if (b0 == 0xEF && b1 == 0xB8 && b2 == 0x8F) return 3; // VS16
    if (b0 == 0xE2 && b1 == 0x80 && b2 == 0x8D) return 3; // ZWJ
    return 0;
}

std::string lint_message(const Rule &r, std::string_view orig) {
    switch (r.w) {
    case W_FILLER: return cat("'", orig, "' is filler and carries no instruction (R1)");
    case W_UNICODE: return cat("'", orig, "' costs extra tokens; use ASCII '", r.to, "'");
    case W_NUMERONYM: return cat("'", orig, "' costs more tokens than '", r.to, "' (R8)");
    case W_ABBREV: return cat("'", orig, "' costs more tokens than '", r.to, "'");
    case W_NUMBER_WORDS: return cat("write '", orig, "' as the digit '", r.to, "' (R10)");
    case W_ARTICLES:
        return cat("article '", orig.substr(0, orig.size() - 1), "' can usually be dropped (R2)");
    default: return {};
    }
}

std::string cleanup(std::string_view s) {
    const Mask m = protect_mask(s);
    constexpr std::string_view kTight = ",;:.)?!";
    std::string b;
    b.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (m[i]) { b += c; continue; }
        char last = b.empty() ? '\0' : b.back();
        if (c == ' ' || c == '\t') {
            if (b.empty() || last == ' ') continue;
            b += ' ';
            continue;
        }
        if (kTight.find(c) != std::string_view::npos && last == ' ' && !b.empty() && !m[i - 1]) {
            b.pop_back();
            last = b.empty() ? '\0' : b.back();
        }
        if (c == ',' && (last == ',' || b.empty())) continue;
        b += c;
    }
    std::size_t e = b.size();
    while (e > 0 && (b[e - 1] == ' ' || b[e - 1] == ',')) e--;
    std::size_t k = 0;
    while (k < e && (b[k] == ' ' || b[k] == ',')) k++;
    return b.substr(k, e - k);
}

// ---------------------------------------------------------------- helpers

bool contains_word(std::string_view hay, std::string_view w) {
    for (std::size_t p = hay.find(w); p != std::string_view::npos; p = hay.find(w, p + 1)) {
        bool before = p == 0 || !is_ident_char(hay[p - 1]);
        bool after = !is_ident_char(at(hay, p + w.size()));
        if (before && after) return true;
    }
    return false;
}

std::string_view kind_text(const Node &n) {
    switch (n.kind) {
    case NodeKind::Plus: return "+";
    case NodeKind::Minus: return "-";
    case NodeKind::Bang: return "!";
    case NodeKind::Step: return "step";
    default: return n.key;
    }
}

bool same_stmt(const Node &a, const Node &b) {
    if (a.kind != b.kind) return false;
    if (a.kind == NodeKind::Field && a.key != b.key) return false;
    if (a.kind == NodeKind::Step) return false;
    return a.value == b.value;
}

Loc value_loc(const Node &n, int len) {
    Loc l = n.loc;
    l.col = n.vcol;
    l.len = len;
    return l;
}

} // namespace

int node_rank(const Node &n) {
    switch (n.kind) {
    case NodeKind::Field: {
        const KeyInfo *k = key_info(n.key);
        return k ? k->rank : 2;
    }
    case NodeKind::Plus:
    case NodeKind::Minus:
    case NodeKind::Bang: return 3;
    case NodeKind::Step: return 4;
    }
    return 2;
}

const char *Sema::suggest_key(std::string_view k) const {
    const char *best = nullptr;
    int bd = 3;
    for (const auto &ki : kKeys) {
        int d = edit_distance(k, ki.name);
        if (d < bd && d < int(k.size())) { bd = d; best = ki.name.data(); }
    }
    for (const auto &c : keys_.all()) {
        int d = edit_distance(k, c);
        if (d < bd && d < int(k.size())) { bd = d; best = c.c_str(); }
    }
    return best;
}

std::string Sema::lint_value(std::string_view v, Loc base, int opt) {
    const Mask mask = protect_mask(v);
    std::string b;
    b.reserve(v.size());
    bool changed = false;
    const std::size_t n = v.size();
    for (std::size_t i = 0; i < n;) {
        if (mask[i]) { b += v[i]; i++; continue; }
        const Rule *hit = nullptr;
        for (const Rule &r : kRules)
            if (rule_matches(r, v, i, mask)) { hit = &r; break; }
        Loc here = base;
        here.col = base.col ? base.col + int(i) : 0;
        if (hit) {
            const std::size_t l = hit->from.size();
            const std::string_view orig = v.substr(i, l);
            here.len = int(l);
            diag_.warning(here, hit->w, lint_message(*hit, orig), std::string(hit->to));
            if (opt >= hit->opt) { b += hit->to; changed = true; }
            else b += orig;
            i += l;
            continue;
        }
        if (std::size_t el = emoji_len(v, i)) {
            std::size_t tot = el, jl;
            while ((jl = joiner_len(v, i + tot)) || (jl = emoji_len(v, i + tot))) tot += jl;
            here.len = 1;
            diag_.warning(here, W_UNICODE, "emoji cost 2-4 tokens and carry no precise meaning; use a word");
            if (opt >= 1) changed = true;
            else b += v.substr(i, tot);
            i += tot;
            continue;
        }
        b += v[i];
        i++;
    }
    if (changed) {
        std::string c = cleanup(b);
        if (c.empty()) return std::string(v); // never optimise a line away to nothing
        return c;
    }
    return b;
}

// ---------------------------------------------------------------- the pass

void Sema::run(Prompt &p, int opt, int errors_before) {
    std::vector<Node> &nodes = p.nodes;
    int max_step = 0;
    bool have_task = false;
    const Node *task = nullptr, *out = nullptr;

    // 1. steps must be numbered 1, 2, 3, ...
    int expect = 1;
    for (const Node &n : nodes) {
        if (n.kind != NodeKind::Step) continue;
        if (n.step != expect)
            diag_.error(n.loc, cat("step ", n.step, " out of sequence; expected step ", expect));
        expect = n.step + 1;
        max_step = std::max(max_step, n.step);
    }

    // 2. statement-level checks
    struct Let { std::string name; const Node *def; };
    std::vector<Let> lets;

    for (std::size_t i = 0; i < nodes.size(); i++) {
        const Node &n = nodes[i];
        if (n.value.empty()) {
            diag_.error(n.loc, cat("expected a value after '", kind_text(n), n.kind == NodeKind::Field ? ":" : "", "'"));
            continue;
        }
        if (n.kind != NodeKind::Field) continue;

        const KeyInfo *ki = key_info(n.key);
        if (!ki && !keys_.contains(n.key)) {
            const char *s = suggest_key(n.key);
            if (s && std::string_view(s) == "task") have_task = true; // don't cascade into 'no task'
            if (s) diag_.error(n.loc, cat("unknown key '", n.key, "'; did you mean '", s, "'?"));
            else diag_.error(n.loc, cat("unknown key '", n.key, "' (declare custom keys with '#pragma key ", n.key, "')"));
            continue;
        }
        if (n.key == "task") have_task = true;
        if (ki && ki->single) {
            for (std::size_t j = 0; j < i; j++) {
                const Node &m = nodes[j];
                if (m.kind == NodeKind::Field && m.key == n.key) {
                    if (ki->single == 2) {
                        diag_.error(n.loc, cat("redefinition of '", n.key, ":'"));
                        diag_.note(m.loc, "previous definition is here");
                    } else {
                        diag_.warning(n.loc, W_REPEAT_KEY, cat("'", n.key, ":' given more than once; the model sees both"));
                        diag_.note(m.loc, "first given here");
                    }
                    break;
                }
            }
        }
        if (n.key == "task" && !task) task = &n;
        if (n.key == "out" && !out) out = &n;

        if (n.key == "let") {
            const std::size_t eq = n.value.find('=');
            std::string name, rhs;
            bool ok = false;
            if (eq != std::string::npos) {
                name = trim(std::string_view(n.value).substr(0, eq));
                rhs = trim(std::string_view(n.value).substr(eq + 1));
                ok = !name.empty() && is_ident_start(name[0]) &&
                     std::all_of(name.begin(), name.end(), [](char c) { return is_ident_char(c) || c == '-'; });
            }
            if (!ok || rhs.empty()) {
                diag_.error(value_loc(n, int(n.value.size())), "expected 'name = value' in 'let:'");
            } else {
                bool dup = false;
                for (const Let &l : lets)
                    if (l.name == name) {
                        diag_.error(value_loc(n, int(name.size())), cat("redefinition of '", name, "'"));
                        diag_.note(l.def->loc, "previous definition is here");
                        dup = true;
                    }
                if (!dup) lets.push_back(Let{name, &n});
            }
        } else if (n.key == "if") {
            if (!unprotected_contains(n.value, "->"))
                diag_.error(value_loc(n, int(n.value.size())), "expected '->' in 'if:' (write it as 'condition -> action')");
        } else if (n.key == "err") {
            char c = n.value[0];
            if (c != '"' && c != '\'' && c != '`')
                diag_.warning(value_loc(n, int(n.value.size())), W_ERR_QUOTE,
                              "error text is not quoted; paste it exactly and wrap it in quotes (R11)");
        }
    }

    // 3. references to steps
    for (const Node &n : nodes) {
        const bool is_ref = n.kind == NodeKind::Field && n.key == "ref";
        if (!is_ref && max_step == 0) continue;
        const std::string_view v = n.value;
        const Mask mask = protect_mask(v);
        for (std::size_t off = 0; off < v.size(); off++) {
            if (mask[off] || !istarts_with(v, off, "step ")) continue;
            if (off > 0 && is_word_char(v[off - 1])) continue;
            if (!ascii::is_digit(at(v, off + 5))) continue;
            std::size_t de = off + 5;
            while (de < v.size() && ascii::is_digit(v[de])) de++;
            const int k = to_int(v.substr(off + 5, de - off - 5));
            if (k < 1 || k > max_step) {
                Loc l = value_loc(n, int(de - off));
                l.col = n.vcol + int(off);
                if (max_step)
                    diag_.error(l, cat("reference to step ", k, ", but the prompt has only ", max_step, " step",
                                       max_step == 1 ? "" : "s"));
                else
                    diag_.error(l, cat("reference to step ", k, ", but the prompt has no numbered steps"));
            } else if (n.kind == NodeKind::Step && k >= n.step) {
                Loc l = value_loc(n, 6);
                l.col = n.vcol + int(off);
                diag_.error(l, cat("step ", n.step, " refers to step ", k, ", which has not run yet"));
            }
        }
    }

    // 4. lint + optimise every value (err: text is never touched)
    for (Node &n : nodes) {
        if (n.kind == NodeKind::Field && n.key == "err") continue;
        Loc base = n.loc;
        base.col = n.subst ? 0 : n.vcol;
        n.value = lint_value(n.value, base, opt);
    }

    // 5. duplicates (R13)
    for (std::size_t i = 0; i < nodes.size(); i++)
        for (std::size_t j = 0; j < i; j++)
            if (!nodes[j].dead && same_stmt(nodes[i], nodes[j])) {
                diag_.warning(nodes[i].loc, W_DUPLICATE, "duplicate line; it repeats an earlier line (R13)");
                diag_.note(nodes[j].loc, "first written here");
                if (opt >= 1) nodes[i].dead = true;
                break;
            }

    // 6. unused let: names
    for (const Let &l : lets) {
        bool used = false;
        for (const Node &n : nodes)
            if (&n != l.def && contains_word(n.value, l.name)) { used = true; break; }
        if (!used)
            diag_.warning(value_loc(*l.def, int(l.name.size())), W_UNUSED_LET,
                          cat("unused name '", l.name, "' (R15: use let: only for things you repeat)"));
    }

    // 7. whole-prompt checks
    if (nodes.empty()) {
        diag_.error(Loc{p.file, 0, 0, 0, nullptr}, "empty prompt");
        return;
    }
    if (!have_task && max_step == 0)
        diag_.error(nodes[0].loc, "prompt has no 'task:' and no numbered steps; every prompt needs one (like main() in C)");
    if (!out) {
        Loc l = task ? task->loc : nodes[0].loc;
        l.col = 0;
        diag_.warning(l, W_NO_OUT, "prompt has no 'out:' line; the model will pick its own format and length (R17)");
    } else {
        bool has_num = std::any_of(out->value.begin(), out->value.end(), [](char c) { return ascii::is_digit(c); });
        static constexpr std::string_view kLengthWords[] = {"terse", "code", "y/n", "json", "changed lines", "1 line", "diff"};
        for (std::string_view kw : kLengthWords)
            if (out->value.find(kw) != std::string::npos) has_num = true;
        if (!has_num)
            diag_.warning(value_loc(*out, int(out->value.size())), W_OUT_LENGTH,
                          "'out:' sets no length; add a number like 'max 100 words' (R18)");
    }
    if (task && task != &nodes[0] && opt < 2)
        diag_.warning(task->loc, W_FIELD_ORDER, "'task:' should be the first line (-O2 reorders)");
    if (out) {
        std::size_t last = nodes.size() - 1;
        while (last > 0 && nodes[last].dead) last--;
        if (out != &nodes[last] && opt < 2)
            diag_.warning(out->loc, W_FIELD_ORDER, "'out:' should be the last line (-O2 reorders)");
    }

    // 8. -O2: canonical order task -> let -> context -> + - ! -> steps -> out (stable)
    if (opt >= 2 && diag_.errors() == errors_before)
        std::stable_sort(nodes.begin(), nodes.end(),
                         [](const Node &a, const Node &b) { return node_rank(a) < node_rank(b); });
}

} // namespace terse
