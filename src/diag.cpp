// diag.cpp — gcc/clang-style diagnostics with source line and caret.
//
//   prompt.terse:3:1: error: unknown key 'tsk'; did you mean 'task'?
//       3 | tsk: summarize article
//         | ^~~
#include "tersec.hpp"

#include <cstdio>

namespace terse {

namespace {

constexpr const char *kWarnNames[W__COUNT] = {
    "filler",       // W_FILLER
    "unicode",      // W_UNICODE
    "numeronym",    // W_NUMERONYM
    "abbrev",       // W_ABBREV
    "number-words", // W_NUMBER_WORDS
    "missing-out",  // W_NO_OUT
    "duplicate",    // W_DUPLICATE
    "unused-let",   // W_UNUSED_LET
    "err-quote",    // W_ERR_QUOTE
    "repeat-key",   // W_REPEAT_KEY
    "redefined",    // W_REDEFINED
    "user",         // W_USER
    "unknown-pragma", // W_UNKNOWN_PRAGMA
    "field-order",  // W_FIELD_ORDER
    "articles",     // W_ARTICLES
    "out-length",   // W_OUT_LENGTH
};

constexpr const char *BOLD = "\033[1m";
constexpr const char *RED = "\033[1;31m";
constexpr const char *MAG = "\033[1;35m";
constexpr const char *CYAN = "\033[1;36m";
constexpr const char *GREEN = "\033[1;32m";
constexpr const char *RESET = "\033[0m";

void put(std::string_view s) { std::fwrite(s.data(), 1, s.size(), stderr); }
void put(char ch) { std::fputc(ch, stderr); }

} // namespace

Diagnostics::Diagnostics() {
    for (int i = 0; i < W__COUNT; i++) enabled_[i] = i < W__EXTRA;
}

bool Diagnostics::option(std::string_view arg) {
    if (arg == "-w") { all_off_ = true; return true; }
    if (arg.substr(0, 2) != "-W") return false;
    std::string_view w = arg.substr(2);
    if (w == "all") {
        for (int i = 0; i < W__EXTRA; i++) enabled_[i] = true;
        return true;
    }
    if (w == "extra") {
        for (bool &e : enabled_) e = true;
        return true;
    }
    if (w == "error") { werror_ = true; return true; }
    bool on = true;
    if (w.substr(0, 3) == "no-") { on = false; w.remove_prefix(3); }
    for (int i = 0; i < W__COUNT; i++)
        if (w == kWarnNames[i]) { enabled_[i] = on; return true; }
    std::fprintf(stderr, "tersec: warning: unknown warning option '%.*s'\n", int(arg.size()), arg.data());
    return true;
}

const Diagnostics::SrcFile &Diagnostics::add_source(std::string name, std::vector<std::string> lines) {
    srcs_.push_back(SrcFile{std::move(name), std::move(lines)});
    return srcs_.back();
}

const std::string *Diagnostics::src_line(std::string_view file, int line) const {
    if (file.empty() || line <= 0) return nullptr;
    for (auto it = srcs_.rbegin(); it != srcs_.rend(); ++it)
        if (it->name == file && line <= int(it->lines.size())) return &it->lines[std::size_t(line - 1)];
    return nullptr;
}

void Diagnostics::print_snippet(const Loc &loc) const {
    const std::string *text = src_line(loc.file, loc.line);
    if (!text) return;
    std::fprintf(stderr, "%5d | ", loc.line);
    for (char ch : *text) put(ch == '\t' ? ' ' : ch);
    put('\n');
    if (loc.col <= 0) return;
    int width = int(text->size());
    int col = loc.col > width + 1 ? width + 1 : loc.col;
    put("      | ");
    put(std::string(std::size_t(col - 1), ' '));
    put(c(GREEN));
    put('^');
    for (int i = 1; i < loc.len && col + i <= width; i++) put('~');
    put(c(RESET));
    put('\n');
}

void Diagnostics::print_loc(const Loc &loc) const {
    put(c(BOLD));
    if (!loc.file.empty()) {
        put(loc.file);
        put(':');
        if (loc.line > 0) put(cat(loc.line, ':'));
        if (loc.line > 0 && loc.col > 0) put(cat(loc.col, ':'));
        put(' ');
    } else {
        put("tersec: ");
    }
    put(c(RESET));
}

std::string Diagnostics::json_object(const Loc &loc, const char *kind, const std::string *flag,
                                     const std::string &msg, const std::optional<std::string> &fixit) const {
    auto num = [](int v) { return v > 0 ? std::to_string(v) : std::string("null"); };
    auto where = [&](const Loc &l) {
        return cat("\"file\": ", l.file.empty() ? std::string("null") : json_quote(l.file), ", \"line\": ",
                   num(l.line), ", \"column\": ", num(l.col));
    };
    std::string o = cat("{\"kind\": ", json_quote(kind), ", ", where(loc), ", \"length\": ",
                        loc.col > 0 ? std::to_string(loc.len > 0 ? loc.len : 1) : std::string("null"),
                        ", \"message\": ", json_quote(msg));
    o += cat(", \"option\": ", flag ? json_quote(flag->substr(flag->rfind(",-W") == std::string::npos ? 0 : flag->rfind(",-W") + 1)) : "null");
    if (fixit && loc.col > 0) o += cat(", \"fixit\": {\"replacement\": ", json_quote(*fixit), "}");
    o += ", \"expansions\": [";
    bool first = true;
    for (const Expansion *e = loc.exp; e; e = e->use.exp) {
        o += cat(first ? "" : ", ", "{", where(e->use), ", \"type\": ",
                 e->kind == Expansion::Kind::Loop ? "\"for\"" : "\"template\"", ", \"name\": ", json_quote(e->name), "}");
        first = false;
    }
    o += "]";
    return o;   // closed by finish(), which may add "notes"
}

void Diagnostics::report(const Loc &loc, const char *kind, const char *kcolor, const std::string *flag,
                         const std::string &msg, const std::optional<std::string> &fixit) {
    if (format_ == Format::Json) {
        std::string obj = json_object(loc, kind, flag, msg, fixit);
        if (std::string_view(kind) == "note" && !json_.empty()) json_.back().notes.push_back(obj + "}");
        else json_.push_back(JsonDiag{obj, {}});
        return;
    }
    print_loc(loc);
    put(cat(c(kcolor), kind, ':', c(RESET), ' ', c(BOLD), msg, c(RESET)));
    if (flag) put(cat(" [", *flag, ']'));
    put('\n');
    print_snippet(loc);
    int shown = 0;
    for (const Expansion *e = loc.exp; e; e = e->use.exp) {
        if (++shown > 3) {
            int more = 0;
            for (; e; e = e->use.exp) more++;
            put(cat(c(CYAN), "note:", c(RESET), " (", more, " more expansion", more == 1 ? "" : "s",
                    " not shown)\n"));
            break;
        }
        print_loc(e->use);
        if (e->kind == Expansion::Kind::Loop)
            put(cat(c(CYAN), "note:", c(RESET), " in '#for' iteration '", e->name, "' here\n"));
        else
            put(cat(c(CYAN), "note:", c(RESET), " in expansion of template '", e->name, "' here\n"));
        print_snippet(e->use);
    }
}

void Diagnostics::error(const Loc &loc, const std::string &msg) {
    report(loc, "error", RED, nullptr, msg);
    notes_muted_ = false;
    errors_++;
}

void Diagnostics::warning(const Loc &loc, Warn w, const std::string &msg, const std::optional<std::string> &fixit) {
    if (!enabled(w)) { notes_muted_ = true; return; }
    notes_muted_ = false;
    std::string flag = cat(werror_ ? "-Werror," : "", "-W", kWarnNames[w]);
    if (werror_) {
        report(loc, "error", RED, &flag, msg, fixit);
        errors_++;
    } else {
        report(loc, "warning", MAG, &flag, msg, fixit);
        warnings_++;
    }
}

void Diagnostics::note(const Loc &loc, const std::string &msg) {
    if (notes_muted_) return;
    report(loc, "note", CYAN, nullptr, msg);
}

void Diagnostics::print_fatal(const std::string &msg) {
    if (format_ == Format::Json) {
        json_.push_back(JsonDiag{json_object(Loc{}, "fatal error", nullptr, msg, std::nullopt), {}});
        finish();
        return;
    }
    put(cat("tersec: ", c(RED), "fatal error:", c(RESET), ' ', msg, '\n'));
}

void Diagnostics::finish() {
    if (finished_) return;
    finished_ = true;
    if (format_ == Format::Json) {
        std::string out = "[";
        for (std::size_t i = 0; i < json_.size(); i++) {
            out += i ? ",\n  " : "\n  ";
            out += json_[i].object + ", \"notes\": [";
            for (std::size_t k = 0; k < json_[i].notes.size(); k++) out += (k ? ", " : "") + json_[i].notes[k];
            out += "]}";
        }
        out += json_.empty() ? "]\n" : "\n]\n";
        put(out);
        return;
    }
    if (!errors_ && !warnings_) return;
    std::string s;
    if (warnings_) s += cat(warnings_, " warning", warnings_ == 1 ? "" : "s");
    if (warnings_ && errors_) s += " and ";
    if (errors_) s += cat(errors_, " error", errors_ == 1 ? "" : "s");
    put(cat(s, " generated.\n"));
}

} // namespace terse
