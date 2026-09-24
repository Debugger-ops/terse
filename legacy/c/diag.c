/* diag.c — gcc/clang-style diagnostics with source line and caret.
 *
 *   prompt.terse:3:1: error: unknown key 'tsk'; did you mean 'task'?
 *       3 | tsk: summarize article
 *         | ^~~
 */
#include "tersec.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *warn_names[W__COUNT] = {
    [W_FILLER] = "filler",
    [W_UNICODE] = "unicode",
    [W_NUMERONYM] = "numeronym",
    [W_ABBREV] = "abbrev",
    [W_NUMBER_WORDS] = "number-words",
    [W_NO_OUT] = "missing-out",
    [W_DUPLICATE] = "duplicate",
    [W_UNUSED_LET] = "unused-let",
    [W_ERR_QUOTE] = "err-quote",
    [W_FIELD_ORDER] = "field-order",
    [W_REPEAT_KEY] = "repeat-key",
    [W_REDEFINED] = "redefined",
    [W_USER] = "user",
    [W_UNKNOWN_PRAGMA] = "unknown-pragma",
    [W_ARTICLES] = "articles",
    [W_OUT_LENGTH] = "out-length",
};

static int enabled[W__COUNT];
static int all_off = 0;
static int werror = 0;
static int color = 0;
static int n_errors = 0, n_warnings = 0;
static int notes_muted = 0; /* notes that follow a silenced warning are silenced too */

typedef struct { const char *file; char **lines; int n; } SrcFile;
static SrcFile *srcs = NULL;
static int n_srcs = 0;

void diag_init(void) {
    for (int i = 0; i < W__COUNT; i++) enabled[i] = 1;
    for (int i = W__EXTRA; i < W__COUNT; i++) enabled[i] = 0;
}

void diag_set_color(int on) { color = on; }

int diag_option(const char *arg) {
    if (strcmp(arg, "-w") == 0) { all_off = 1; return 1; }
    if (strncmp(arg, "-W", 2) != 0) return 0;
    const char *w = arg + 2;
    if (strcmp(w, "all") == 0) {
        for (int i = 0; i < W__EXTRA; i++) enabled[i] = 1;
        return 1;
    }
    if (strcmp(w, "extra") == 0) {
        for (int i = 0; i < W__COUNT; i++) enabled[i] = 1;
        return 1;
    }
    if (strcmp(w, "error") == 0) { werror = 1; return 1; }
    int on = 1;
    if (strncmp(w, "no-", 3) == 0) { on = 0; w += 3; }
    for (int i = 0; i < W__COUNT; i++)
        if (strcmp(w, warn_names[i]) == 0) { enabled[i] = on; return 1; }
    fprintf(stderr, "tersec: warning: unknown warning option '%s'\n", arg);
    return 1;
}

int diag_warn_enabled(WarnId w) { return !all_off && enabled[w]; }

void srcmap_add(const char *file, char **lines, int n) {
    srcs = xrealloc(srcs, sizeof(SrcFile) * (size_t)(n_srcs + 1));
    srcs[n_srcs++] = (SrcFile){file, lines, n};
}

static const char *src_line(const char *file, int line) {
    if (!file || line <= 0) return NULL;
    for (int i = n_srcs - 1; i >= 0; i--)
        if (strcmp(srcs[i].file, file) == 0 && line <= srcs[i].n) return srcs[i].lines[line - 1];
    return NULL;
}

#define C(code) (color ? code : "")
#define BOLD C("\033[1m")
#define RED C("\033[1;31m")
#define MAG C("\033[1;35m")
#define CYAN C("\033[1;36m")
#define GREEN C("\033[1;32m")
#define RESET C("\033[0m")

static void print_snippet(Loc loc) {
    const char *text = src_line(loc.file, loc.line);
    if (!text) return;
    fprintf(stderr, "%5d | ", loc.line);
    for (const char *p = text; *p; p++) fputc(*p == '\t' ? ' ' : *p, stderr);
    fputc('\n', stderr);
    if (loc.col <= 0) return;
    int width = (int)strlen(text);
    int col = loc.col > width + 1 ? width + 1 : loc.col;
    fprintf(stderr, "      | ");
    for (int i = 1; i < col; i++) fputc(' ', stderr);
    fprintf(stderr, "%s^", GREEN);
    for (int i = 1; i < loc.len && col + i <= width; i++) fputc('~', stderr);
    fprintf(stderr, "%s\n", RESET);
}

static void print_loc(Loc loc) {
    fprintf(stderr, "%s", BOLD);
    if (loc.file) {
        fprintf(stderr, "%s:", loc.file);
        if (loc.line > 0) fprintf(stderr, "%d:", loc.line);
        if (loc.line > 0 && loc.col > 0) fprintf(stderr, "%d:", loc.col);
        fputc(' ', stderr);
    } else {
        fprintf(stderr, "tersec: ");
    }
    fprintf(stderr, "%s", RESET);
}

static void report(Loc loc, const char *kind, const char *kcolor, const char *flag,
                   const char *fmt, va_list ap) {
    print_loc(loc);
    fprintf(stderr, "%s%s:%s %s", kcolor, kind, RESET, BOLD);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s", RESET);
    if (flag) fprintf(stderr, " [%s]", flag);
    fputc('\n', stderr);
    print_snippet(loc);
    int shown = 0;
    for (const Expansion *e = loc.exp; e; e = e->use.exp) {
        if (++shown > 3) {
            int more = 0;
            for (; e; e = e->use.exp) more++;
            fprintf(stderr, "%snote:%s (%d more expansion%s not shown)\n", CYAN, RESET, more, more == 1 ? "" : "s");
            break;
        }
        print_loc(e->use);
        fprintf(stderr, "%snote:%s in expansion of template '%s' here\n", CYAN, RESET, e->name);
        print_snippet(e->use);
    }
}

void error_at(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, "error", RED, NULL, fmt, ap);
    va_end(ap);
    notes_muted = 0;
    n_errors++;
}

void warning_at(Loc loc, WarnId w, const char *fmt, ...) {
    if (!diag_warn_enabled(w)) { notes_muted = 1; return; }
    notes_muted = 0;
    char flag[64];
    snprintf(flag, sizeof flag, "%s-W%s", werror ? "-Werror," : "", warn_names[w]);
    va_list ap;
    va_start(ap, fmt);
    if (werror) {
        report(loc, "error", RED, flag, fmt, ap);
        n_errors++;
    } else {
        report(loc, "warning", MAG, flag, fmt, ap);
        n_warnings++;
    }
    va_end(ap);
}

void note_at(Loc loc, const char *fmt, ...) {
    if (notes_muted) return;
    va_list ap;
    va_start(ap, fmt);
    report(loc, "note", CYAN, NULL, fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "tersec: %sfatal error:%s ", RED, RESET);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

int diag_errors(void) { return n_errors; }
int diag_warnings(void) { return n_warnings; }

void diag_summary(void) {
    if (!n_errors && !n_warnings) return;
    Buf b;
    buf_init(&b);
    if (n_warnings) buf_printf(&b, "%d warning%s", n_warnings, n_warnings == 1 ? "" : "s");
    if (n_warnings && n_errors) buf_puts(&b, " and ");
    if (n_errors) buf_printf(&b, "%d error%s", n_errors, n_errors == 1 ? "" : "s");
    fprintf(stderr, "%s generated.\n", b.data);
    free(b.data);
}
