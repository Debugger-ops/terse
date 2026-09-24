// pp.cpp — the Terse preprocessor.
//
// Works like the C preprocessor, line by line:
//   // and block comments           stripped (// only at line start or after a space,
//                                   so URLs like https://x survive)
//   #include "file.terse"           textual include, relative to the current file, then -I dirs
//   #pragma once                   include this file at most once
//   #pragma key NAME               allow a custom field key NAME:
//   #define NAME value             compile-time constant, used as {NAME}
//   #undef NAME
//   #ifdef / #ifndef / #if / #elif / #else / #endif
//   #template name(a, b) ... #end  parameterised block, like a function
//   #template name(a, b="default")  parameters may have defaults (trailing ones only)
//   #use name(x, y)                expand a template
//   #for x in a, b, "c, d" ... #end  repeat a block once per item, with {x} bound
//   #pragma budget N               error if the compiled prompt is over ~N tokens
//   #error msg / #warning msg
//
// Text inside `backticks` and ``` code fences is never touched.
#include "tersec.hpp"

#include <filesystem>
#include <system_error>

namespace terse {

namespace {

constexpr int kMaxDepth = 64;

Loc at_col(Loc loc, int col, int len) {
    loc.col = col;
    loc.len = len > 0 ? len : 1;
    return loc;
}

// Length of the identifier at the start of s (0 if none).
std::size_t read_ident(std::string_view s) {
    if (s.empty() || !is_ident_start(s[0])) return 0;
    std::size_t i = 0;
    while (i < s.size() && is_ident_char(s[i])) i++;
    return i;
}

std::size_t skip_blanks(std::string_view s, std::size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

std::size_t skip_spaces(std::string_view s, std::size_t i) {
    while (i < s.size() && s[i] == ' ') i++;
    return i;
}

bool directive_is(std::string_view trimmed, std::string_view name) {
    if (trimmed.empty() || trimmed[0] != '#') return false;
    std::size_t p = skip_blanks(trimmed, 1);
    return trimmed.compare(p, name.size(), name) == 0 && trimmed.size() - p >= name.size() &&
           !is_ident_char(at(trimmed, p + name.size()));
}


// Splits "a, b, \"c, d\"" on commas outside double quotes; trims and unquotes each part.
std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (std::size_t i = 0; i <= s.size(); i++) {
        char c = at(s, i);
        if (c == '"') in_q = !in_q;
        if (i == s.size() || (!in_q && c == ',')) {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(strip_quotes(t));
            cur.clear();
            continue;
        }
        cur += c;
    }
    return out;
}

} // namespace

// Replaces comments with spaces (so columns stay put) and drops // to end of line.
std::string strip_comments(std::string_view s, bool &in_block) {
    std::string b;
    b.reserve(s.size());
    bool in_quote = false, in_tick = false;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (in_block) {
            if (c == '*' && at(s, i + 1) == '/') {
                in_block = false;
                b += "  ";
                i++;
            } else {
                b += c == '\t' ? '\t' : ' ';
            }
            continue;
        }
        if (!in_tick && c == '"') in_quote = !in_quote;
        if (!in_quote && c == '`') in_tick = !in_tick;
        if (!in_quote && !in_tick) {
            if (c == '/' && at(s, i + 1) == '*') {
                in_block = true;
                b += "  ";
                i++;
                continue;
            }
            if (c == '/' && at(s, i + 1) == '/' && (i == 0 || ascii::is_space(s[i - 1]))) break;
        }
        b += c;
    }
    while (!b.empty() && ascii::is_space(b.back())) b.pop_back();
    return b;
}

std::size_t Preprocessor::Template::min_args() const {
    std::size_t n = 0;
    while (n < defaults.size() && !defaults[n]) n++;
    return n;
}

// ---------------------------------------------------------------- definitions

void Preprocessor::set_def(DefMap &defs, const std::string &name, const std::string &value, const Loc &loc) {
    auto it = defs.find(name);
    if (it != defs.end()) {
        Define &d = it->second;
        if (d.value != value && !loc.file.empty()) {
            diag_.warning(loc, W_REDEFINED, cat("'", name, "' redefined"));
            if (!d.loc.file.empty()) diag_.note(d.loc, "previous definition is here");
        }
        d.value = value;
        d.loc = loc;
        return;
    }
    defs.emplace(name, Define{value, loc});
}

void Preprocessor::define(const std::string &name, const std::string &value) {
    set_def(cli_defs_, name, value, Loc{});
}

void Preprocessor::undef(const std::string &name) { cli_defs_.erase(name); }

const Preprocessor::Define *Preprocessor::find_def(const std::string &name) const {
    auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : &it->second;
}

const Preprocessor::Template *Preprocessor::find_tpl(std::string_view name) const {
    for (const auto &t : templates_)
        if (t.name == name) return &t;
    return nullptr;
}

const std::string *Preprocessor::lookup(const std::string &name, const Params *pa) const {
    for (const Params *p = pa; p; p = p->parent)
        for (std::size_t i = 0; i < p->names.size(); i++)
            if (p->names[i] == name) return &p->values[i];
    const Define *d = find_def(name);
    return d ? &d->value : nullptr;
}

bool Preprocessor::truthy(const std::string &name) const {
    const Define *d = find_def(name);
    if (!d) return false;
    return !(d->value.empty() || d->value == "0" || d->value == "false");
}

// ---------------------------------------------------------------- {NAME} substitution

std::string Preprocessor::substitute(std::string_view s, const Loc &loc, int col0, const Params *pa, int lvl) {
    std::string b;
    b.reserve(s.size());
    bool in_tick = false;
    for (std::size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '`') { in_tick = !in_tick; b += c; continue; }
        if (in_tick) { b += c; continue; }
        if (c == '\\' && at(s, i + 1) == '{') { b += '{'; i++; continue; }
        if (c == '{' && is_ident_start(at(s, i + 1))) {
            std::size_t j = i + 1;
            while (j < s.size() && is_ident_char(s[j])) j++;
            if (at(s, j) == '}') {
                std::string name(s.substr(i + 1, j - i - 1));
                std::string_view whole = s.substr(i, j - i + 1);
                Loc here = at_col(loc, col0 + int(i) + 1, int(whole.size()));
                if (const std::string *val = lookup(name, pa)) {
                    if (lvl > 16) {
                        diag_.error(here, cat("expansion of '", name, "' is too deep (recursive #define?)"));
                        b += whole;
                    } else {
                        b += substitute(*val, loc, 0, pa, lvl + 1);
                    }
                } else {
                    if (col0 >= 0)
                        diag_.error(here, cat("use of undeclared name '", name, "' (define it with '#define ",
                                              name, " ...', or write '\\{' for a literal brace)"));
                    b += whole;
                }
                i = j;
                continue;
            }
        }
        b += c;
    }
    return b;
}

// ---------------------------------------------------------------- conditions

bool Preprocessor::eval_if(std::string_view expr, const Loc &loc) {
    const std::string e = trim(expr);
    const std::string_view ev = e;
    bool neg = false, r = false;
    std::size_t p = 0;
    if (at(ev, p) == '!') { neg = true; p = skip_spaces(ev, p + 1); }
    std::size_t n = read_ident(ev.substr(p));
    if (!n) {
        diag_.error(loc, "expected a name after '#if'");
        return false;
    }
    std::string name(ev.substr(p, n));
    p = skip_spaces(ev, p + n);
    if (p >= ev.size()) {
        r = truthy(name);
    } else if ((ev[p] == '=' || ev[p] == '!') && at(ev, p + 1) == '=') {
        bool want_eq = ev[p] == '=';
        if (at(ev, p + 2) == '=') diag_.error(loc, "use '==' (two '=') in '#if'");
        std::string rhs = strip_quotes(trim(ev.substr(p + 2)));
        const Define *d = find_def(name);
        bool eq = d && d->value == rhs;
        r = want_eq ? eq : !eq;
    } else {
        diag_.error(loc, "expected '==' or '!=' in '#if' (only 'NAME', '!NAME', 'NAME == value' "
                         "and 'NAME != value' are allowed)");
    }
    return neg ? !r : r;
}

// ---------------------------------------------------------------- templates

void Preprocessor::parse_template_header(std::string_view rest, const Loc &loc, Template &t) {
    t = Template{};
    t.loc = loc;
    std::size_t n = read_ident(rest);
    if (!n) {
        diag_.error(loc, "expected template name after '#template'");
        t.name = "?";
        return;
    }
    t.name = std::string(rest.substr(0, n));
    std::size_t p = skip_spaces(rest, n);
    if (p >= rest.size()) return;
    if (rest[p] != '(') { diag_.error(loc, "expected '(' after template name"); return; }
    p++;
    for (;;) {
        p = skip_spaces(rest, p);
        if (at(rest, p) == ')') { p++; break; }
        n = read_ident(rest.substr(p));
        if (!n) {
            diag_.error(loc, cat("expected parameter name in template '", t.name, "'"));
            return;
        }
        std::string pname(rest.substr(p, n));
        p = skip_spaces(rest, p + n);
        std::optional<std::string> def;
        if (at(rest, p) == '=') {
            // default value: up to ',' or ')' outside quotes
            std::size_t q = p + 1;
            bool in_q = false;
            while (q < rest.size() && (in_q || (rest[q] != ',' && rest[q] != ')'))) {
                if (rest[q] == '"') in_q = !in_q;
                q++;
            }
            def = strip_quotes(trim(rest.substr(p + 1, q - p - 1)));
            p = q;
        } else if (!t.defaults.empty() && t.defaults.back()) {
            diag_.error(loc, cat("parameter '", pname, "' of template '", t.name,
                                 "' needs a default, because an earlier parameter has one"));
        }
        t.params.push_back(std::move(pname));
        t.defaults.push_back(std::move(def));
        if (at(rest, p) == ',') { p++; continue; }
        if (at(rest, p) == ')') { p++; break; }
        diag_.error(loc, cat("expected ',' or ')' in parameter list of template '", t.name, "'"));
        return;
    }
    p = skip_spaces(rest, p);
    if (p < rest.size()) diag_.error(loc, "unexpected text after template parameter list");
}

void Preprocessor::expand_use(std::string_view rest, const Loc &loc, const Params *pa) {
    std::size_t n = read_ident(rest);
    if (!n) { diag_.error(loc, "expected template name after '#use'"); return; }
    std::string name(rest.substr(0, n));
    const Template *t = find_tpl(name);
    std::vector<std::string> args;
    std::size_t p = skip_spaces(rest, n);
    if (at(rest, p) == '(') {
        p++;
        std::string cur;
        bool in_q = false, closed = false, any = false;
        for (; p < rest.size(); p++) {
            char c = rest[p];
            if (c == '"') in_q = !in_q;
            if (!in_q && (c == ',' || c == ')')) {
                std::string a = strip_quotes(trim(cur));
                cur.clear();
                if (c == ',' || any || !a.empty()) args.push_back(std::move(a));
                if (c == ')') { closed = true; p++; break; }
                any = true;
                continue;
            }
            cur += c;
        }
        if (!closed) {
            diag_.error(loc, cat("expected ')' to close arguments of '", name, "'"));
            return;
        }
        p = skip_spaces(rest, p);
        if (p < rest.size()) diag_.error(loc, "unexpected text after ')' in '#use'");
    } else if (p < rest.size()) {
        diag_.error(loc, "expected '(' after template name in '#use'");
    }
    if (!t) {
        diag_.error(loc, cat("use of undeclared template '", name, "'"));
        return;
    }
    const std::size_t lo = t->min_args(), hi = t->params.size();
    if (args.size() < lo || args.size() > hi) {
        const bool few = args.size() < lo;
        const char *range = lo == hi ? "" : few ? "at least " : "at most ";
        diag_.error(loc, cat("too ", few ? "few" : "many", " arguments to template '", name, "': expected ", range,
                             few ? lo : hi, ", have ", args.size()));
        diag_.note(t->loc, cat("'", name, "' declared here"));
        return;
    }
    if (depth_ >= kMaxDepth) {
        diag_.error(loc, cat("template '", name, "' nested too deeply (recursive template?)"));
        return;
    }
    for (auto &a : args) a = substitute(a, loc, -1, pa, 0);
    // missing trailing arguments take their defaults; {NAME}s in them resolve inside the body
    for (std::size_t i = args.size(); i < hi; i++) args.push_back(*t->defaults[i]);

    expansions_.push_back(Expansion{loc, name});
    const Expansion *e = &expansions_.back();
    // Copies: the expansion may define new templates, and we must not hold references into them.
    const std::vector<std::string> params = t->params;
    std::vector<RLine> body = t->body;
    for (auto &l : body) l.loc.exp = e;
    Params np{params, args};
    depth_++;
    pp_lines(body, &np);
    depth_--;
}

// ---------------------------------------------------------------- #for loops

void Preprocessor::parse_for_header(std::string_view rest, int rest_col, const Loc &loc, const Params *pa, Loop &loop) {
    std::size_t n = read_ident(rest);
    if (!n) {
        diag_.error(loc, "expected a loop variable after '#for'");
        return;
    }
    loop.var = std::string(rest.substr(0, n));
    std::size_t p = skip_blanks(rest, n);
    if (rest.compare(p, 2, "in") != 0 || is_ident_char(at(rest, p + 2))) {
        diag_.error(loc, cat("expected 'in' after '#for ", loop.var, "'"));
        return;
    }
    // substitute first, so '#for x in {LIST}' works with '#define LIST a, b, c'
    loop.items = split_list(substitute(rest.substr(p + 2), loc, rest_col + int(p) + 2, pa, 0));
}

void Preprocessor::finish_block(Block &b, const Params *pa) {
    if (b.discard) return;
    if (!b.is_loop) {
        if (const Template *old = find_tpl(b.tpl.name)) {
            diag_.error(b.tpl.loc, cat("redefinition of template '", b.tpl.name, "'"));
            diag_.note(old->loc, "previous definition is here");
        } else {
            templates_.push_back(std::move(b.tpl));
        }
        return;
    }
    if (depth_ >= kMaxDepth) {
        diag_.error(b.loc, "'#for' nested too deeply");
        return;
    }
    const std::vector<std::string> names{b.loop.var};
    for (const std::string &item : b.loop.items) {
        expansions_.push_back(Expansion{b.loc, cat(b.loop.var, " = ", item), Expansion::Kind::Loop});
        const Expansion *e = &expansions_.back();
        std::vector<RLine> body = b.tpl.body;
        for (auto &l : body) l.loc.exp = e;
        const std::vector<std::string> values{item};
        Params np{names, values, pa};
        depth_++;
        pp_lines(body, &np);
        depth_--;
    }
}

// ---------------------------------------------------------------- files

void Preprocessor::do_include(std::string_view rest, const Loc &loc) {
    const std::string arg = trim(rest);
    const std::size_t n = arg.size();
    bool angle;
    if (n >= 2 && arg[0] == '"' && arg[n - 1] == '"') angle = false;
    else if (n >= 2 && arg[0] == '<' && arg[n - 1] == '>') angle = true;
    else { diag_.error(loc, "#include expects \"FILENAME\" or <FILENAME>"); return; }
    const std::string name = arg.substr(1, n - 2);

    std::optional<std::string> found;
    if (!name.empty() && name[0] == '/') {
        if (file_exists(name)) found = name;
    } else {
        if (!angle && !loc.file.empty()) {
            std::string cand;
            std::size_t slash = loc.file.rfind('/');
            if (slash != std::string_view::npos) cand = std::string(loc.file.substr(0, slash + 1));
            cand += name;
            if (file_exists(cand)) found = std::move(cand);
        }
        for (const auto &dir : incdirs_) {
            if (found) break;
            std::string cand = cat(dir, '/', name);
            if (file_exists(cand)) found = std::move(cand);
        }
    }
    if (!found) {
        diag_.error(loc, cat("'", name, "' file not found"));
        return;
    }
    if (depth_ >= kMaxDepth) {
        diag_.error(loc, "#include nested too deeply");
        return;
    }
    depth_++;
    pp_file(*found, *found, &loc);
    depth_--;
}

bool Preprocessor::is_once(const std::string &path) const {
    std::error_code ec;
    auto real = std::filesystem::canonical(path, ec);
    return !ec && once_.count(real.string());
}

void Preprocessor::mark_once(std::string_view path) {
    if (path.empty()) return;
    std::error_code ec;
    auto real = std::filesystem::canonical(std::string(path), ec);
    if (!ec) once_.insert(real.string());
}

void Preprocessor::pp_file(const std::string &path, const std::string &shown, const Loc *from) {
    if (path != "-" && is_once(path)) return;
    std::optional<std::string> src = read_file(path);
    if (!src) {
        if (from) diag_.error(*from, cat("cannot read '", path, "'"));
        else throw FatalError(cat(shown, ": No such file or directory"));
        return;
    }
    std::string_view v = *src;
    v = v.substr(0, v.find('\0'));   // text ends at the first NUL byte
    if (v.size() >= 3 && v[0] == '\xEF' && v[1] == '\xBB' && v[2] == '\xBF') v.remove_prefix(3);

    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos < v.size();) {
        std::size_t e = v.find('\n', pos);
        std::size_t l = e == std::string_view::npos ? v.size() - pos : e - pos;
        std::size_t ll = l;
        if (ll > 0 && v[pos + ll - 1] == '\r') ll--;
        lines.emplace_back(v.substr(pos, ll));
        pos += l;
        if (pos < v.size() && v[pos] == '\n') pos++;
    }

    const Diagnostics::SrcFile &sf = diag_.add_source(shown, std::move(lines));
    if (!from) main_source_ = &sf;
    std::vector<RLine> rl;
    rl.reserve(sf.lines.size());
    for (std::size_t i = 0; i < sf.lines.size(); i++)
        rl.push_back(RLine{sf.lines[i], Loc{sf.name, int(i) + 1, 0, 0, nullptr}});
    pp_lines(rl, nullptr);
}

// ---------------------------------------------------------------- the line loop

void Preprocessor::pp_lines(const std::vector<RLine> &lines, const Params *pa) {
    std::vector<Cond> conds;
    bool in_block = false, in_fence = false;
    Loc fence_loc{};
    std::optional<Block> collecting;

    for (const RLine &line : lines) {
        const std::string_view text = line.text;
        const Loc &loc = line.loc;
        const bool active = conds.empty() ? true : conds.back().active;
        const std::string_view tp = text.substr(skip_blanks(text, 0));

        // collecting a #template or #for body up to its matching #end
        if (collecting) {
            if (directive_is(tp, "end") && collecting->nest == 0) {
                Block b = std::move(*collecting);
                collecting.reset();
                finish_block(b, pa);
            } else if (directive_is(tp, "template")) {
                diag_.error(loc, collecting->is_loop
                                     ? std::string("'#template' cannot be defined inside '#for'")
                                     : cat("'#template' cannot be nested; missing '#end' for '", collecting->tpl.name, "'?"));
            } else {
                if (directive_is(tp, "for")) collecting->nest++;
                if (directive_is(tp, "end")) collecting->nest--;
                collecting->tpl.body.push_back(line);
            }
            continue;
        }

        // inside a ``` fence: pass through untouched
        if (in_fence) {
            if (active) out_.push_back(PLine{std::string(text), loc, true, false});
            if (count_substr(text, "```") % 2 == 1) in_fence = false;
            continue;
        }

        const std::string s = strip_comments(text, in_block);
        const std::string_view sv = s;
        const std::size_t p = skip_blanks(sv, 0);
        if (p == sv.size()) continue;

        if (sv[p] == '#') {
            const std::size_t q = skip_blanks(sv, p + 1);
            const std::size_t nl = read_ident(sv.substr(q));
            const std::string dname(sv.substr(q, nl));
            const std::string_view rest = sv.substr(skip_blanks(sv, q + nl));
            const Loc dloc = at_col(loc, int(p) + 1, int(q - p + nl));

            if (dname == "ifdef" || dname == "ifndef" || dname == "if") {
                bool v = false;
                if (active) {
                    if (dname == "if") {
                        v = eval_if(rest, dloc);
                    } else {
                        std::size_t in = read_ident(rest);
                        if (!in) diag_.error(dloc, cat("expected a name after '#", dname, "'"));
                        v = find_def(std::string(rest.substr(0, in))) != nullptr;
                        if (dname == "ifndef") v = !v;
                    }
                }
                conds.push_back(Cond{active, v, active && v, false, dloc});
            } else if (dname == "elif") {
                if (conds.empty()) {
                    diag_.error(dloc, "#elif without #if");
                } else {
                    Cond &c = conds.back();
                    if (c.else_seen) diag_.error(dloc, "#elif after #else");
                    if (c.taken || !c.parent_active) {
                        c.active = false;
                    } else {
                        bool v = eval_if(rest, dloc);
                        c.active = v;
                        c.taken = v;
                    }
                }
            } else if (dname == "else") {
                if (conds.empty()) {
                    diag_.error(dloc, "#else without #if");
                } else {
                    Cond &c = conds.back();
                    if (c.else_seen) diag_.error(dloc, "#else after #else");
                    c.else_seen = true;
                    c.active = c.parent_active && !c.taken;
                    c.taken = true;
                }
            } else if (dname == "endif") {
                if (conds.empty()) diag_.error(dloc, "#endif without #if");
                else conds.pop_back();
            } else if (dname == "template") {
                collecting.emplace();
                collecting->loc = dloc;
                int errs = diag_.errors();
                parse_template_header(rest, dloc, collecting->tpl);
                collecting->discard = !active || diag_.errors() > errs;
            } else if (dname == "for") {
                collecting.emplace();
                collecting->is_loop = true;
                collecting->loc = dloc;
                collecting->tpl.name = "#for";
                int errs = diag_.errors();
                if (active) parse_for_header(rest, int(rest.data() - sv.data()), dloc, pa, collecting->loop);
                collecting->discard = !active || diag_.errors() > errs;
            } else if (dname == "end") {
                if (active) diag_.error(dloc, "#end without #template or #for");
            } else if (!active) {
                // skipped region: ignore everything else
            } else if (dname == "include") {
                do_include(rest, dloc);
            } else if (dname == "define") {
                std::size_t in = read_ident(rest);
                if (!in) diag_.error(dloc, "macro name must be an identifier");
                else set_def(defs_, std::string(rest.substr(0, in)), trim(rest.substr(in)), dloc);
            } else if (dname == "undef") {
                std::size_t in = read_ident(rest);
                if (!in) diag_.error(dloc, "macro name must be an identifier");
                else defs_.erase(std::string(rest.substr(0, in)));
            } else if (dname == "use") {
                expand_use(rest, dloc, pa);
            } else if (dname == "pragma") {
                std::size_t in = read_ident(rest);
                if (in == 4 && rest.substr(0, 4) == "once") {
                    if (!loc.exp) mark_once(loc.file);
                } else if (in == 6 && rest.substr(0, 6) == "budget") {
                    std::size_t k = skip_spaces(rest, 6), d = k;
                    while (d < rest.size() && ascii::is_digit(rest[d])) d++;
                    if (d == k || skip_blanks(rest, d) != rest.size())
                        diag_.error(dloc, "expected a token count after '#pragma budget', like '#pragma budget 200'");
                    else
                        budget_ = Budget{to_int(rest.substr(k, d - k)), dloc};
                } else if (in == 3 && rest.substr(0, 3) == "key") {
                    std::size_t k = skip_spaces(rest, 3);
                    std::size_t kn = read_ident(rest.substr(k));
                    if (!kn) diag_.error(dloc, "expected key name after '#pragma key'");
                    else keys_.add(std::string(rest.substr(k, kn)));
                } else {
                    diag_.warning(dloc, W_UNKNOWN_PRAGMA, "unknown pragma ignored");
                }
            } else if (dname == "error") {
                diag_.error(dloc, substitute(rest, loc, -1, pa, 0));
            } else if (dname == "warning") {
                diag_.warning(dloc, W_USER, substitute(rest, loc, -1, pa, 0));
            } else {
                diag_.error(dloc, cat("invalid preprocessing directive '#", dname, "'"));
            }
            continue;
        }

        if (!active) continue;

        std::string t = substitute(sv, loc, 0, pa, 0);
        const bool changed = t != s;
        if (count_substr(t, "```") % 2 == 1) {
            in_fence = true;
            fence_loc = loc;
            fence_loc.col = int(t.find("```")) + 1;
            fence_loc.len = 3;
        }
        out_.push_back(PLine{std::move(t), loc, false, changed});
    }

    if (collecting) {
        if (collecting->is_loop)
            diag_.error(collecting->loc, cat("unterminated '#for ", collecting->loop.var, "' (missing '#end')"));
        else
            diag_.error(collecting->tpl.loc, cat("unterminated '#template ", collecting->tpl.name, "' (missing '#end')"));
    }
    if (in_fence) diag_.error(fence_loc, "unterminated code fence (missing closing ```)");
    if (in_block && !lines.empty()) diag_.error(lines.back().loc, "unterminated /* comment");
    for (auto it = conds.rbegin(); it != conds.rend(); ++it) diag_.error(it->loc, "unterminated conditional directive");
}

PLines Preprocessor::run(const std::string &path) {
    defs_ = cli_defs_;
    templates_.clear();
    once_.clear();
    out_.clear();
    depth_ = 0;
    budget_.reset();
    main_source_ = nullptr;
    pp_file(path, path == "-" ? "<stdin>" : path, nullptr);
    return std::move(out_);
}

} // namespace terse
