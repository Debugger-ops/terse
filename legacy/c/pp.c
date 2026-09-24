/* pp.c — the Terse preprocessor.
 *
 * Works like the C preprocessor, line by line:
 *   // and block comments           stripped (// only at line start or after a space,
 *                                   so URLs like https://x survive)
 *   #include "file.terse"           textual include, relative to the current file, then -I dirs
 *   #pragma once                   include this file at most once
 *   #pragma key NAME               allow a custom field key NAME:
 *   #define NAME value             compile-time constant, used as {NAME}
 *   #undef NAME
 *   #ifdef / #ifndef / #if / #elif / #else / #endif
 *   #template name(a, b) ... #end  parameterised block, like a function
 *   #use name(x, y)                expand a template
 *   #error msg / #warning msg
 *
 * Text inside `backticks` and ``` code fences is never touched.
 */
#define _DEFAULT_SOURCE
#include "tersec.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *text; Loc loc; } RLine;

typedef struct {
    char *name;
    char *value;
    Loc loc;
} Define;

typedef struct {
    char *name;
    char **params;
    int nparams;
    RLine *body;
    int nbody;
    Loc loc;
} Template;

typedef struct {
    char **names;
    char **values;
    int n;
} Params;

typedef struct {
    int parent_active;
    int taken;
    int active;
    int else_seen;
    Loc loc;
} Cond;

static Define *defs;
static int ndefs;
static Template *tpls;
static int ntpls;
static char **incdirs;
static int nincdirs;
static char **once_files;
static int nonce;
static PLines *out;
static int depth;

#define MAX_DEPTH 64

/* ---------- small helpers ---------- */

static void push_line(char *text, Loc loc, int raw) {
    if (out->n == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 64;
        out->v = xrealloc(out->v, sizeof(PLine) * (size_t)out->cap);
    }
    out->v[out->n++] = (PLine){text, loc, raw, 0};
}

static Define *find_def(const char *name) {
    for (int i = ndefs - 1; i >= 0; i--)
        if (strcmp(defs[i].name, name) == 0) return &defs[i];
    return NULL;
}

static Template *find_tpl(const char *name) {
    for (int i = 0; i < ntpls; i++)
        if (strcmp(tpls[i].name, name) == 0) return &tpls[i];
    return NULL;
}

static void set_def(const char *name, const char *value, Loc loc) {
    Define *d = find_def(name);
    if (d) {
        if (strcmp(d->value, value) != 0 && loc.file) {
            warning_at(loc, W_REDEFINED, "'%s' redefined", name);
            if (d->loc.file) note_at(d->loc, "previous definition is here");
        }
        free(d->value);
        d->value = xstrdup(value);
        d->loc = loc;
        return;
    }
    defs = xrealloc(defs, sizeof(Define) * (size_t)(ndefs + 1));
    defs[ndefs++] = (Define){xstrdup(name), xstrdup(value), loc};
}

void pp_define(const char *name, const char *value) {
    set_def(name, value ? value : "1", (Loc){0});
}

void pp_undef(const char *name) {
    for (int i = 0; i < ndefs; i++)
        if (strcmp(defs[i].name, name) == 0) {
            defs[i] = defs[--ndefs];
            return;
        }
}

void pp_add_include_dir(const char *dir) {
    incdirs = xrealloc(incdirs, sizeof(char *) * (size_t)(nincdirs + 1));
    incdirs[nincdirs++] = xstrdup(dir);
}

static Loc at_col(Loc loc, int col, int len) {
    loc.col = col;
    loc.len = len > 0 ? len : 1;
    return loc;
}

static char *strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
    return s;
}

/* ---------- comments ---------- */

static char *strip_comments(const char *s, int *in_block) {
    Buf b;
    buf_init(&b);
    int in_quote = 0, in_tick = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (*in_block) {
            if (c == '*' && s[i + 1] == '/') {
                *in_block = 0;
                buf_puts(&b, "  ");
                i++;
            } else {
                buf_putc(&b, c == '\t' ? '\t' : ' ');
            }
            continue;
        }
        if (!in_tick && c == '"') in_quote = !in_quote;
        if (!in_quote && c == '`') in_tick = !in_tick;
        if (!in_quote && !in_tick) {
            if (c == '/' && s[i + 1] == '*') {
                *in_block = 1;
                buf_puts(&b, "  ");
                i++;
                continue;
            }
            if (c == '/' && s[i + 1] == '/' && (i == 0 || isspace((unsigned char)s[i - 1]))) break;
        }
        buf_putc(&b, c);
    }
    char *r = buf_take(&b);
    size_t n = strlen(r);
    while (n > 0 && isspace((unsigned char)r[n - 1])) r[--n] = '\0';
    return r;
}

/* ---------- {NAME} substitution ---------- */

static const char *lookup(const char *name, const Params *pa) {
    for (const Params *p = pa; p; p = NULL)
        for (int i = 0; i < p->n; i++)
            if (strcmp(p->names[i], name) == 0) return p->values[i];
    Define *d = find_def(name);
    return d ? d->value : NULL;
}

static char *substitute(const char *s, Loc loc, int col0, const Params *pa, int lvl) {
    Buf b;
    buf_init(&b);
    int in_tick = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '`') { in_tick = !in_tick; buf_putc(&b, c); continue; }
        if (in_tick) { buf_putc(&b, c); continue; }
        if (c == '\\' && s[i + 1] == '{') { buf_putc(&b, '{'); i++; continue; }
        if (c == '{' && is_ident_start((unsigned char)s[i + 1])) {
            size_t j = i + 1;
            while (is_ident_char((unsigned char)s[j])) j++;
            if (s[j] == '}') {
                char *name = xstrndup(s + i + 1, j - i - 1);
                const char *val = lookup(name, pa);
                if (val) {
                    if (lvl > 16) {
                        error_at(at_col(loc, col0 + (int)i + 1, (int)(j - i + 1)),
                                 "expansion of '%s' is too deep (recursive #define?)", name);
                        buf_putn(&b, s + i, j - i + 1);
                    } else {
                        char *v = substitute(val, loc, 0, pa, lvl + 1);
                        buf_puts(&b, v);
                        free(v);
                    }
                } else {
                    if (col0 >= 0)
                        error_at(at_col(loc, col0 + (int)i + 1, (int)(j - i + 1)),
                                 "use of undeclared name '%s' (define it with '#define %s ...', "
                                 "or write '\\{' for a literal brace)", name, name);
                    buf_putn(&b, s + i, j - i + 1);
                }
                free(name);
                i = j;
                continue;
            }
        }
        buf_putc(&b, c);
    }
    return buf_take(&b);
}

/* ---------- conditions ---------- */

static int truthy(const char *name) {
    Define *d = find_def(name);
    if (!d) return 0;
    return !(d->value[0] == '\0' || strcmp(d->value, "0") == 0 || strcmp(d->value, "false") == 0);
}

static int read_ident(const char *s, size_t *n) {
    size_t i = 0;
    if (!is_ident_start((unsigned char)s[0])) { *n = 0; return 0; }
    while (is_ident_char((unsigned char)s[i])) i++;
    *n = i;
    return 1;
}

static int eval_if(const char *expr, Loc loc) {
    char *e = xtrim(expr);
    int neg = 0, r = 0;
    const char *p = e;
    if (*p == '!') { neg = 1; p++; while (*p == ' ') p++; }
    size_t n;
    if (!read_ident(p, &n)) {
        error_at(loc, "expected a name after '#if'");
        free(e);
        return 0;
    }
    char *name = xstrndup(p, n);
    p += n;
    while (*p == ' ') p++;
    if (*p == '\0') {
        r = truthy(name);
    } else if ((p[0] == '=' || p[0] == '!') && p[1] == '=') {
        int want_eq = p[0] == '=';
        if (p[2] == '=') error_at(loc, "use '==' (two '=') in '#if'");
        char *rhs = strip_quotes(xtrim(p + 2));
        Define *d = find_def(name);
        int eq = d && strcmp(d->value, rhs) == 0;
        r = want_eq ? eq : !eq;
        free(rhs);
    } else {
        error_at(loc, "expected '==' or '!=' in '#if' (only 'NAME', '!NAME', 'NAME == value' "
                      "and 'NAME != value' are allowed)");
    }
    free(name);
    free(e);
    return neg ? !r : r;
}

/* ---------- templates ---------- */

static void parse_template_header(const char *rest, Loc loc, Template *t) {
    size_t n;
    memset(t, 0, sizeof *t);
    t->loc = loc;
    if (!read_ident(rest, &n)) {
        error_at(loc, "expected template name after '#template'");
        t->name = xstrdup("?");
        return;
    }
    t->name = xstrndup(rest, n);
    const char *p = rest + n;
    while (*p == ' ') p++;
    if (*p == '\0') return;
    if (*p != '(') { error_at(loc, "expected '(' after template name"); return; }
    p++;
    for (;;) {
        while (*p == ' ') p++;
        if (*p == ')') { p++; break; }
        if (!read_ident(p, &n)) { error_at(loc, "expected parameter name in template '%s'", t->name); return; }
        t->params = xrealloc(t->params, sizeof(char *) * (size_t)(t->nparams + 1));
        t->params[t->nparams++] = xstrndup(p, n);
        p += n;
        while (*p == ' ') p++;
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        error_at(loc, "expected ',' or ')' in parameter list of template '%s'", t->name);
        return;
    }
    while (*p == ' ') p++;
    if (*p) error_at(loc, "unexpected text after template parameter list");
}

static void pp_lines(RLine *lines, int n, const Params *pa, const Expansion *exp);

static void expand_use(const char *rest, Loc loc, const Params *pa) {
    size_t n;
    if (!read_ident(rest, &n)) { error_at(loc, "expected template name after '#use'"); return; }
    char *name = xstrndup(rest, n);
    Template *t = find_tpl(name);
    const char *p = rest + n;
    char **args = NULL;
    int nargs = 0;
    while (*p == ' ') p++;
    if (*p == '(') {
        p++;
        Buf cur;
        buf_init(&cur);
        int in_q = 0, closed = 0, any = 0;
        for (; *p; p++) {
            if (*p == '"') in_q = !in_q;
            if (!in_q && (*p == ',' || *p == ')')) {
                char *a = strip_quotes(xtrim(cur.data ? cur.data : ""));
                free(cur.data);
                buf_init(&cur);
                if (*p == ',' || any || a[0]) {
                    args = xrealloc(args, sizeof(char *) * (size_t)(nargs + 1));
                    args[nargs++] = a;
                } else {
                    free(a);
                }
                if (*p == ')') { closed = 1; p++; break; }
                any = 1;
                continue;
            }
            buf_putc(&cur, *p);
        }
        free(cur.data);
        if (!closed) { error_at(loc, "expected ')' to close arguments of '%s'", name); free(name); return; }
        while (*p == ' ') p++;
        if (*p) error_at(loc, "unexpected text after ')' in '#use'");
    } else if (*p) {
        error_at(loc, "expected '(' after template name in '#use'");
    }
    if (!t) {
        error_at(loc, "use of undeclared template '%s'", name);
        free(name);
        return;
    }
    if (nargs != t->nparams) {
        error_at(loc, "too %s arguments to template '%s': expected %d, have %d",
                 nargs < t->nparams ? "few" : "many", name, t->nparams, nargs);
        note_at(t->loc, "'%s' declared here", name);
        free(name);
        return;
    }
    if (depth >= MAX_DEPTH) {
        error_at(loc, "template '%s' nested too deeply (recursive template?)", name);
        free(name);
        return;
    }
    for (int i = 0; i < nargs; i++) {
        char *s = substitute(args[i], loc, -1, pa, 0);
        free(args[i]);
        args[i] = s;
    }
    Expansion *e = xmalloc(sizeof *e);
    e->use = loc;
    e->name = name;
    RLine *body = xmalloc(sizeof(RLine) * (size_t)(t->nbody + 1));
    for (int i = 0; i < t->nbody; i++) {
        body[i] = t->body[i];
        body[i].loc.exp = e;
    }
    Params np = {t->params, args, nargs};
    depth++;
    pp_lines(body, t->nbody, &np, e);
    depth--;
    free(body);
}

/* ---------- files ---------- */

static void pp_file(const char *path, const char *shown, const Loc *from);

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void do_include(const char *rest, Loc loc) {
    char *arg = xtrim(rest);
    size_t n = strlen(arg);
    int angle;
    if (n >= 2 && arg[0] == '"' && arg[n - 1] == '"') angle = 0;
    else if (n >= 2 && arg[0] == '<' && arg[n - 1] == '>') angle = 1;
    else { error_at(loc, "#include expects \"FILENAME\" or <FILENAME>"); free(arg); return; }
    char *name = xstrndup(arg + 1, n - 2);
    free(arg);
    char *found = NULL;
    Buf b;
    buf_init(&b);
    if (name[0] == '/') {
        if (file_exists(name)) found = xstrdup(name);
    } else {
        if (!angle && loc.file) {
            const char *slash = strrchr(loc.file, '/');
            if (slash) buf_putn(&b, loc.file, (size_t)(slash - loc.file + 1));
            buf_puts(&b, name);
            if (file_exists(b.data)) found = buf_take(&b);
            else { free(b.data); buf_init(&b); }
        }
        for (int i = 0; !found && i < nincdirs; i++) {
            buf_printf(&b, "%s/%s", incdirs[i], name);
            if (file_exists(b.data)) found = buf_take(&b);
            else { free(b.data); buf_init(&b); }
        }
    }
    if (!found) {
        error_at(loc, "'%s' file not found", name);
        free(name);
        return;
    }
    if (depth >= MAX_DEPTH) {
        error_at(loc, "#include nested too deeply");
        free(name);
        return;
    }
    depth++;
    pp_file(found, found, &loc);
    depth--;
    free(name);
}

static int is_once(const char *path) {
    char real[PATH_MAX];
    if (!realpath(path, real)) return 0;
    for (int i = 0; i < nonce; i++)
        if (strcmp(once_files[i], real) == 0) return 1;
    return 0;
}

static void mark_once(const char *path) {
    char real[PATH_MAX];
    if (!path || !realpath(path, real) || is_once(path)) return;
    once_files = xrealloc(once_files, sizeof(char *) * (size_t)(nonce + 1));
    once_files[nonce++] = xstrdup(real);
}

/* ---------- the line loop ---------- */

static int directive_is(const char *trimmed, const char *name) {
    size_t n = strlen(name);
    if (trimmed[0] != '#') return 0;
    const char *p = trimmed + 1;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, name, n) == 0 && !is_ident_char((unsigned char)p[n]);
}

static void pp_lines(RLine *lines, int n, const Params *pa, const Expansion *exp) {
    Cond *conds = NULL;
    int nconds = 0;
    int in_block = 0, in_fence = 0;
    Loc fence_loc = {0};
    Template *collecting = NULL;
    int collect_discard = 0;
    (void)exp;

    for (int li = 0; li < n; li++) {
        const char *text = lines[li].text;
        Loc loc = lines[li].loc;
        int active = nconds ? conds[nconds - 1].active : 1;

        const char *tp = text;
        while (*tp == ' ' || *tp == '\t') tp++;

        /* collecting a #template body */
        if (collecting) {
            if (directive_is(tp, "end")) {
                if (!collect_discard) {
                    Template *old = find_tpl(collecting->name);
                    if (old) {
                        error_at(collecting->loc, "redefinition of template '%s'", collecting->name);
                        note_at(old->loc, "previous definition is here");
                    } else {
                        tpls = xrealloc(tpls, sizeof(Template) * (size_t)(ntpls + 1));
                        tpls[ntpls++] = *collecting;
                    }
                }
                free(collecting);
                collecting = NULL;
            } else if (directive_is(tp, "template")) {
                error_at(loc, "'#template' cannot be nested; missing '#end' for '%s'?", collecting->name);
            } else {
                collecting->body = xrealloc(collecting->body, sizeof(RLine) * (size_t)(collecting->nbody + 1));
                collecting->body[collecting->nbody++] = lines[li];
            }
            continue;
        }

        /* inside a ``` fence: pass through untouched */
        if (in_fence) {
            if (active) push_line(xstrdup(text), loc, 1);
            if (count_substr(text, "```") % 2 == 1) in_fence = 0;
            continue;
        }

        char *s = strip_comments(text, &in_block);
        const char *p = s;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') { free(s); continue; }

        if (*p == '#') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            size_t nl;
            read_ident(q, &nl);
            char *dname = xstrndup(q, nl);
            const char *rest = q + nl;
            while (*rest == ' ' || *rest == '\t') rest++;
            Loc dloc = at_col(loc, (int)(p - s) + 1, (int)(q - p + nl));

            if (!strcmp(dname, "ifdef") || !strcmp(dname, "ifndef") || !strcmp(dname, "if")) {
                int v = 0;
                if (active) {
                    if (!strcmp(dname, "if")) v = eval_if(rest, dloc);
                    else {
                        size_t in_;
                        if (!read_ident(rest, &in_)) error_at(dloc, "expected a name after '#%s'", dname);
                        char *nm = xstrndup(rest, in_);
                        v = find_def(nm) != NULL;
                        if (!strcmp(dname, "ifndef")) v = !v;
                        free(nm);
                    }
                }
                conds = xrealloc(conds, sizeof(Cond) * (size_t)(nconds + 1));
                conds[nconds++] = (Cond){active, v, active && v, 0, dloc};
            } else if (!strcmp(dname, "elif")) {
                if (!nconds) error_at(dloc, "#elif without #if");
                else {
                    Cond *c = &conds[nconds - 1];
                    if (c->else_seen) error_at(dloc, "#elif after #else");
                    if (c->taken || !c->parent_active) c->active = 0;
                    else { int v = eval_if(rest, dloc); c->active = v; c->taken = v; }
                }
            } else if (!strcmp(dname, "else")) {
                if (!nconds) error_at(dloc, "#else without #if");
                else {
                    Cond *c = &conds[nconds - 1];
                    if (c->else_seen) error_at(dloc, "#else after #else");
                    c->else_seen = 1;
                    c->active = c->parent_active && !c->taken;
                    c->taken = 1;
                }
            } else if (!strcmp(dname, "endif")) {
                if (!nconds) error_at(dloc, "#endif without #if");
                else nconds--;
            } else if (!strcmp(dname, "template")) {
                collecting = xmalloc(sizeof(Template));
                int errs = diag_errors();
                parse_template_header(rest, dloc, collecting);
                collect_discard = !active || diag_errors() > errs;
            } else if (!strcmp(dname, "end")) {
                if (active) error_at(dloc, "#end without #template");
            } else if (!active) {
                /* skipped region: ignore everything else */
            } else if (!strcmp(dname, "include")) {
                do_include(rest, dloc);
            } else if (!strcmp(dname, "define")) {
                size_t in_;
                if (!read_ident(rest, &in_)) error_at(dloc, "macro name must be an identifier");
                else {
                    char *nm = xstrndup(rest, in_);
                    const char *v = rest + in_;
                    char *val = xtrim(v);
                    set_def(nm, val, dloc);
                    free(nm);
                    free(val);
                }
            } else if (!strcmp(dname, "undef")) {
                size_t in_;
                if (!read_ident(rest, &in_)) error_at(dloc, "macro name must be an identifier");
                else { char *nm = xstrndup(rest, in_); pp_undef(nm); free(nm); }
            } else if (!strcmp(dname, "use")) {
                expand_use(rest, dloc, pa);
            } else if (!strcmp(dname, "pragma")) {
                size_t in_;
                read_ident(rest, &in_);
                if (in_ == 4 && !strncmp(rest, "once", 4)) {
                    if (!loc.exp) mark_once(loc.file);
                } else if (in_ == 3 && !strncmp(rest, "key", 3)) {
                    const char *k = rest + 3;
                    while (*k == ' ') k++;
                    size_t kn;
                    if (!read_ident(k, &kn)) error_at(dloc, "expected key name after '#pragma key'");
                    else { char *kk = xstrndup(k, kn); sema_add_custom_key(kk); free(kk); }
                } else {
                    warning_at(dloc, W_UNKNOWN_PRAGMA, "unknown pragma ignored");
                }
            } else if (!strcmp(dname, "error")) {
                char *m = substitute(rest, loc, -1, pa, 0);
                error_at(dloc, "%s", m);
                free(m);
            } else if (!strcmp(dname, "warning")) {
                char *m = substitute(rest, loc, -1, pa, 0);
                warning_at(dloc, W_USER, "%s", m);
                free(m);
            } else {
                error_at(dloc, "invalid preprocessing directive '#%s'", dname);
            }
            free(dname);
            free(s);
            continue;
        }

        if (!active) { free(s); continue; }

        char *t = substitute(s, loc, 0, pa, 0);
        int changed = strcmp(s, t) != 0;
        free(s);
        if (count_substr(t, "```") % 2 == 1) {
            in_fence = 1;
            fence_loc = loc;
            fence_loc.col = (int)(strstr(t, "```") - t) + 1;
            fence_loc.len = 3;
        }
        push_line(t, loc, 0);
        out->v[out->n - 1].subst = changed;
    }

    if (collecting) {
        error_at(collecting->loc, "unterminated '#template %s' (missing '#end')", collecting->name);
        free(collecting);
    }
    if (in_fence) error_at(fence_loc, "unterminated code fence (missing closing ```)");
    if (in_block && n > 0) {
        Loc l = lines[n - 1].loc;
        error_at(l, "unterminated /* comment");
    }
    for (int i = nconds - 1; i >= 0; i--) error_at(conds[i].loc, "unterminated conditional directive");
    free(conds);
}

static void pp_file(const char *path, const char *shown, const Loc *from) {
    if (strcmp(path, "-") != 0 && is_once(path)) return;
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        if (from) error_at(*from, "cannot read '%s'", path);
        else fatal("%s: No such file or directory", shown);
        return;
    }
    const char *start = src;
    if ((unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) start += 3;

    char **lines = NULL;
    int n = 0;
    const char *p = start;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        size_t ll = l;
        if (ll > 0 && p[ll - 1] == '\r') ll--;
        lines = xrealloc(lines, sizeof(char *) * (size_t)(n + 1));
        lines[n++] = xstrndup(p, ll);
        p += l;
        if (*p == '\n') p++;
    }
    free(src);

    const char *fname = xstrdup(shown);
    srcmap_add(fname, lines, n);
    RLine *rl = xmalloc(sizeof(RLine) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) rl[i] = (RLine){lines[i], {fname, i + 1, 0, 0, NULL}};
    pp_lines(rl, n, NULL, NULL);
    free(rl);
}

PLines *pp_run(const char *path) {
    /* each input file is its own translation unit: only -D names carry over */
    static int cli_ndefs = -1;
    if (cli_ndefs < 0) cli_ndefs = ndefs;
    ndefs = cli_ndefs;
    ntpls = 0;
    nonce = 0;
    out = xcalloc(1, sizeof(PLines));
    depth = 0;
    pp_file(path, strcmp(path, "-") == 0 ? "<stdin>" : path, NULL);
    return out;
}
