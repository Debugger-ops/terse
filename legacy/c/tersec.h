/* tersec — the Terse prompt compiler.
 * Shared declarations for every stage of the pipeline:
 *
 *   source.terse -> [preprocess] -> PLines -> [parse] -> Prompt
 *               -> [sema + lint + optimize] -> [emit] -> prompt text / json / english
 */
#ifndef TERSEC_H
#define TERSEC_H

#include <stddef.h>
#include <stdio.h>

#define TERSEC_VERSION "0.2.0"
#define TERSE_LANG_VERSION "0.2"

/* ---------- util.c ---------- */

typedef struct { char *data; size_t len, cap; } Buf;

void  buf_init(Buf *b);
void  buf_putc(Buf *b, char c);
void  buf_putn(Buf *b, const char *s, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_printf(Buf *b, const char *fmt, ...);
char *buf_take(Buf *b);                 /* returns heap string, resets buffer */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xtrim(const char *s);             /* heap copy without surrounding whitespace */
char *read_file(const char *path, size_t *len);

int   is_ident_start(int c);
int   is_ident_char(int c);
int   is_word_char(int c);              /* letters, digits, '_' and any UTF-8 byte */
int   edit_distance(const char *a, const char *b);
int   count_substr(const char *s, const char *sub);
int   str_ieq_n(const char *a, const char *b, size_t n);  /* ASCII case-insensitive */

/* ---------- source locations & diagnostics (diag.c) ---------- */

struct Expansion;

typedef struct Loc {
    const char *file;
    int line;                     /* 1-based; 0 = no line            */
    int col;                      /* 1-based; 0 = no caret           */
    int len;                      /* caret underline length (>=1)    */
    const struct Expansion *exp;  /* template expansion this came from */
} Loc;

typedef struct Expansion {
    Loc use;                      /* where '#use name(...)' was written */
    const char *name;
} Expansion;

typedef enum {
    W_FILLER, W_UNICODE, W_NUMERONYM, W_ABBREV, W_NUMBER_WORDS,
    W_NO_OUT, W_DUPLICATE, W_UNUSED_LET, W_ERR_QUOTE,
    W_REPEAT_KEY, W_REDEFINED, W_USER, W_UNKNOWN_PRAGMA,
    /* off unless -Wextra (W__EXTRA marks the first one) */
    W_FIELD_ORDER, W_ARTICLES, W_OUT_LENGTH,
    W__COUNT
} WarnId;
#define W__EXTRA W_FIELD_ORDER

void diag_init(void);
void diag_set_color(int on);
int  diag_option(const char *arg);      /* handles -W..., -w, returns 1 if consumed */
int  diag_warn_enabled(WarnId w);
void srcmap_add(const char *file, char **lines, int n);

void error_at(Loc loc, const char *fmt, ...);
void warning_at(Loc loc, WarnId w, const char *fmt, ...);
void note_at(Loc loc, const char *fmt, ...);
void fatal(const char *fmt, ...);
int  diag_errors(void);
int  diag_warnings(void);
void diag_summary(void);

/* ---------- preprocessor (pp.c) ---------- */

typedef struct {
    char *text;
    Loc loc;
    int raw;                      /* inside a ``` code fence: passed through untouched */
    int subst;                    /* text changed by {NAME} substitution: columns are approximate */
} PLine;

typedef struct { PLine *v; int n, cap; } PLines;

void    pp_add_include_dir(const char *dir);
void    pp_define(const char *name, const char *value);
void    pp_undef(const char *name);
PLines *pp_run(const char *path);       /* path "-" = stdin */

/* ---------- parser (parse.c) ---------- */

typedef enum { N_FIELD, N_PLUS, N_MINUS, N_BANG, N_STEP } NodeKind;

typedef struct {
    NodeKind kind;
    char *key;                    /* N_FIELD only */
    int step;                     /* N_STEP only  */
    char *value;
    Loc loc;                      /* start of line */
    int vcol;                     /* column where the value starts */
    int dead;                     /* removed by the optimizer */
    int subst;                    /* value came from {NAME} substitution */
} Node;

typedef struct { Node *v; int n, cap; const char *file; } Prompt;

Prompt *parse(PLines *pl, const char *file);

/* ---------- semantic analysis, lint, optimizer (sema.c) ---------- */

void sema_add_custom_key(const char *name);
void sema_run(Prompt *p, int opt_level);
int  node_rank(const Node *n);

/* ---------- emitters (emit.c) ---------- */

typedef enum { EMIT_TERSE, EMIT_JSON, EMIT_ENGLISH } EmitKind;

char *emit(const Prompt *p, EmitKind kind, int with_primer);
int   approx_tokens(const char *s);
extern const char *TERSE_PRIMER;

#endif
