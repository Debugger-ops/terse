// tersec — the Terse prompt compiler (C++17).
// Shared declarations for every stage of the pipeline:
//
//   source.terse -> [Preprocessor] -> PLines -> [parse] -> Prompt
//               -> [Sema: checks + lint + optimize] -> [emit] -> prompt text / json / english
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terse {

inline constexpr const char *kCompilerVersion = "0.4.0";
inline constexpr const char *kLanguageVersion = "0.4";

// ---------------------------------------------------------------- util.cpp

// ASCII-only character classes (locale independent, like C's "C" locale).
namespace ascii {
constexpr bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
constexpr bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
constexpr bool is_punct(char c) { return c > ' ' && c < 0x7f && !is_alnum(c); }
constexpr char to_lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
constexpr char to_upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
} // namespace ascii

constexpr bool is_ident_start(char c) { return ascii::is_alpha(c) || c == '_'; }
constexpr bool is_ident_char(char c) { return ascii::is_alnum(c) || c == '_'; }
// letters, digits, '_' and any UTF-8 byte
constexpr bool is_word_char(char c) { return ascii::is_alnum(c) || c == '_' || (static_cast<unsigned char>(c) & 0x80); }

// s[i], or '\0' past the end (mirrors reading a NUL-terminated C string).
constexpr char at(std::string_view s, std::size_t i) { return i < s.size() ? s[i] : '\0'; }

std::string trim(std::string_view s);         // drop surrounding ' ' '\t' (and trailing '\r' '\n')
std::string strip_quotes(std::string s);      // "x" -> x
std::optional<std::string> read_file(const std::string &path); // "-" = stdin
bool file_exists(const std::string &path);
int  edit_distance(std::string_view a, std::string_view b);    // ASCII case-insensitive
int  count_substr(std::string_view s, std::string_view sub);   // non-overlapping
bool istarts_with(std::string_view s, std::size_t pos, std::string_view prefix);
int  to_int(std::string_view digits);         // like atoi() on a run of digits
std::string json_quote(std::string_view s);   // "..." with JSON escapes

// A file split into lines, remembering each line's ending so it can be written back unchanged.
struct SourceText {
    std::string bom;                          // "\xEF\xBB\xBF" or ""
    std::vector<std::string> lines;           // without line endings
    std::vector<std::string> eols;            // "\n", "\r\n" or "" (last line)
    std::string join() const;
};
SourceText split_source(std::string_view raw);
bool write_file(const std::string &path, std::string_view data);

// cat("a", 1, 'b', str) -> "a1b..."
namespace detail {
inline void append(std::string &s, std::string_view v) { s.append(v); }
inline void append(std::string &s, char c) { s.push_back(c); }
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char> &&
                                        !std::is_same_v<T, bool>, int> = 0>
void append(std::string &s, T v) { s += std::to_string(v); }
} // namespace detail

template <class... Args> std::string cat(const Args &...args) {
    std::string s;
    (detail::append(s, args), ...);
    return s;
}

// ------------------------------------------ source locations & diagnostics (diag.cpp)

struct Expansion;

struct Loc {
    std::string_view file;            // empty = no file
    int line = 0;                     // 1-based; 0 = no line
    int col = 0;                      // 1-based; 0 = no caret
    int len = 0;                      // caret underline length
    const Expansion *exp = nullptr;   // template expansion this came from
};

struct Expansion {
    enum class Kind { Template, Loop };
    Loc use;                          // where '#use name(...)' or '#for' was written
    std::string name;                 // template name, or "x = item" for a loop
    Kind kind = Kind::Template;
};

enum Warn : int {
    W_FILLER, W_UNICODE, W_NUMERONYM, W_ABBREV, W_NUMBER_WORDS,
    W_NO_OUT, W_DUPLICATE, W_UNUSED_LET, W_ERR_QUOTE,
    W_REPEAT_KEY, W_REDEFINED, W_USER, W_UNKNOWN_PRAGMA,
    // off unless -Wextra (W__EXTRA marks the first one)
    W_FIELD_ORDER, W_ARTICLES, W_OUT_LENGTH,
    W__COUNT
};
inline constexpr int W__EXTRA = W_FIELD_ORDER;

struct FatalError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Diagnostics {
public:
    struct SrcFile {
        std::string name;
        std::vector<std::string> lines;
    };

    enum class Format { Text, Json };

    Diagnostics();

    void set_color(bool on) { color_ = on; }
    void set_format(Format f) { format_ = f; }
    Format format() const { return format_; }
    bool option(std::string_view arg);            // -W..., -w; true if consumed
    bool enabled(Warn w) const { return !all_off_ && enabled_[w]; }

    // Registers a file's lines for snippets. The returned reference stays valid for the
    // lifetime of this object, so string_views into it are safe.
    const SrcFile &add_source(std::string name, std::vector<std::string> lines);

    void error(const Loc &loc, const std::string &msg);
    // fixit: replacement text for the underlined range (shown in JSON diagnostics)
    void warning(const Loc &loc, Warn w, const std::string &msg, const std::optional<std::string> &fixit = {});
    void note(const Loc &loc, const std::string &msg);
    void print_fatal(const std::string &msg);

    int  errors() const { return errors_; }
    int  warnings() const { return warnings_; }
    void finish();                    // "N warnings generated." or the JSON array

private:
    const char *c(const char *code) const { return color_ ? code : ""; }
    const std::string *src_line(std::string_view file, int line) const;
    void print_loc(const Loc &loc) const;
    void print_snippet(const Loc &loc) const;
    void report(const Loc &loc, const char *kind, const char *kcolor, const std::string *flag,
                const std::string &msg, const std::optional<std::string> &fixit = {});
    std::string json_object(const Loc &loc, const char *kind, const std::string *flag, const std::string &msg,
                            const std::optional<std::string> &fixit) const;

    bool enabled_[W__COUNT];
    bool all_off_ = false, werror_ = false, color_ = false;
    bool notes_muted_ = false;        // notes after a silenced warning are silenced too
    bool finished_ = false;
    Format format_ = Format::Text;
    struct JsonDiag { std::string object; std::vector<std::string> notes; };
    std::vector<JsonDiag> json_;      // rendered eagerly: Locs may not outlive the compile
    int errors_ = 0, warnings_ = 0;
    std::deque<SrcFile> srcs_;        // deque: references stay valid as it grows
};

// Custom keys declared with '#pragma key NAME'. Shared by the preprocessor and sema.
class KeyRegistry {
public:
    void add(std::string name);
    bool contains(std::string_view name) const;
    const std::vector<std::string> &all() const { return keys_; }
private:
    std::vector<std::string> keys_;
};

// ---------------------------------------------------------- preprocessor (pp.cpp)

struct PLine {
    std::string text;
    Loc loc;
    bool raw = false;                 // inside a ``` code fence: passed through untouched
    bool subst = false;               // changed by {NAME} substitution: columns are approximate
};
using PLines = std::vector<PLine>;

// Blanks out /* */ comments (keeping columns) and cuts // comments; tracks block state across lines.
std::string strip_comments(std::string_view s, bool &in_block);

class Preprocessor {
public:
    struct Budget { int tokens; Loc loc; };

    Preprocessor(Diagnostics &diag, KeyRegistry &keys) : diag_(diag), keys_(keys) {}

    void add_include_dir(std::string dir) { incdirs_.push_back(std::move(dir)); }
    void define(const std::string &name, const std::string &value);   // -D
    void undef(const std::string &name);                              // -U

    // Each call is its own translation unit: only -D/-U names carry over.
    PLines run(const std::string &path);                              // "-" = stdin

    // Set by '#pragma budget N' during the last run (the last one wins).
    const std::optional<Budget> &budget() const { return budget_; }
    // The top-level file of the last run (nullptr if it could not be read).
    const Diagnostics::SrcFile *main_source() const { return main_source_; }

private:
    struct RLine { std::string_view text; Loc loc; };
    struct Define { std::string value; Loc loc; };
    struct Template {
        std::string name;
        std::vector<std::string> params;
        std::vector<std::optional<std::string>> defaults;   // one per param
        std::vector<RLine> body;
        Loc loc;
        std::size_t min_args() const;
    };
    struct Loop {
        std::string var;
        std::vector<std::string> items;
    };
    // A '#template' or '#for' body being collected up to its matching '#end'.
    struct Block {
        bool is_loop = false;
        Template tpl;                 // is_loop == false
        Loop loop;                    // is_loop == true
        Loc loc;
        int nest = 0;                 // '#for' blocks opened inside the body
        bool discard = false;
    };
    struct Params {
        const std::vector<std::string> &names;
        const std::vector<std::string> &values;
        const Params *parent = nullptr;   // loops see the enclosing template's parameters
    };
    struct Cond { bool parent_active, taken, active, else_seen; Loc loc; };
    using DefMap = std::unordered_map<std::string, Define>;

    void set_def(DefMap &defs, const std::string &name, const std::string &value, const Loc &loc);
    const Define *find_def(const std::string &name) const;
    const Template *find_tpl(std::string_view name) const;
    const std::string *lookup(const std::string &name, const Params *pa) const;
    bool truthy(const std::string &name) const;

    std::string substitute(std::string_view s, const Loc &loc, int col0, const Params *pa, int lvl);
    bool eval_if(std::string_view expr, const Loc &loc);
    void parse_template_header(std::string_view rest, const Loc &loc, Template &t);
    void expand_use(std::string_view rest, const Loc &loc, const Params *pa);
    void parse_for_header(std::string_view rest, int rest_col, const Loc &loc, const Params *pa, Loop &loop);
    void finish_block(Block &b, const Params *pa);
    void do_include(std::string_view rest, const Loc &loc);
    bool is_once(const std::string &path) const;
    void mark_once(std::string_view path);
    void pp_file(const std::string &path, const std::string &shown, const Loc *from);
    void pp_lines(const std::vector<RLine> &lines, const Params *pa);

    Diagnostics &diag_;
    KeyRegistry &keys_;
    DefMap cli_defs_, defs_;
    std::deque<Template> templates_;
    std::vector<std::string> incdirs_;
    std::unordered_set<std::string> once_;
    std::deque<Expansion> expansions_;   // kept for the whole run: Locs point into it
    PLines out_;
    int depth_ = 0;
    std::optional<Budget> budget_;
    const Diagnostics::SrcFile *main_source_ = nullptr;
};

// ---------------------------------------------------------------- parser (parse.cpp)

enum class NodeKind { Field, Plus, Minus, Bang, Step };

struct Node {
    NodeKind kind = NodeKind::Field;
    std::string key;                  // Field only
    int step = 0;                     // Step only
    std::string value;
    std::string orig_value;           // value as parsed, before lint rewrites (for --fix)
    Loc loc;                          // start of line
    int vcol = 0;                     // column where the value starts
    bool dead = false;                // removed by the optimizer
    bool subst = false;               // value came from {NAME} substitution
};

struct Prompt {
    std::vector<Node> nodes;
    std::string_view file;
};

Prompt parse(const PLines &lines, std::string_view file, Diagnostics &diag);

// ------------------------------------ semantic analysis, lint, optimizer (sema.cpp)

int node_rank(const Node &n);

class Sema {
public:
    Sema(Diagnostics &diag, const KeyRegistry &keys) : diag_(diag), keys_(keys) {}
    // errors_before: diagnostics error count when this file started (for -O2 reordering).
    void run(Prompt &p, int opt_level, int errors_before);

private:
    std::string lint_value(std::string_view v, Loc base, int opt);
    const char *suggest_key(std::string_view k) const;

    Diagnostics &diag_;
    const KeyRegistry &keys_;
};

// ---------------------------------------------------------------- emitters (emit.cpp)

enum class EmitKind { Terse, Json, English, Xml, Markdown, Yaml };

std::string emit(const Prompt &p, EmitKind kind, bool with_primer);
std::string terse_line(const Node &n);        // one statement in canonical Terse, with '\n'
int approx_tokens(std::string_view s);
extern const char *const kPrimer;

// ---------------------------------------------------------------- formatter (fmt.cpp)

// Canonical layout for Terse source: one space after markers, no trailing blanks,
// single blank lines, 2-space indent inside #if/#template/#for blocks. Fences and
// comments are kept byte-for-byte. Idempotent.
std::string format_source(std::string_view raw);

// ---------------------------------------------------------------- --fix (fix.cpp)

struct FixResult {
    std::string text;                 // the rewritten file
    std::string diff;                 // unified diff (no context lines)
    int changes = 0;
};

// Writes the optimizer's rewrites (and removed duplicate lines) back into the top-level
// source. Only touches single-line values written directly in that file.
FixResult apply_fixes(const Prompt &p, const Diagnostics::SrcFile &src, std::string_view raw);

} // namespace terse
