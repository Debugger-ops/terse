/* sema.c — semantic checks, lint warnings and the optimizer.
 *
 * Errors (the prompt is rejected, like a C compile error):
 *   unknown key, empty value, redefinition of task:/out:, malformed let:,
 *   'if:' without '->', steps out of sequence, reference to a missing step,
 *   prompt with neither task: nor steps.
 *
 * Warnings (-W<name>, all fixable by the optimizer where marked):
 *   filler*, unicode*, numeronym*, abbrev*, number-words*, duplicate*,
 *   missing-out, unused-let, err-quote, repeat-key, redefined, user, unknown-pragma;
 *   with -Wextra: field-order, articles* (fixed at -O2), out-length.
 */
#include "tersec.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------- keys ---------- */

typedef struct {
    const char *name;
    int rank;     /* canonical order */
    int single;   /* 2 = error if repeated, 1 = warning if repeated */
} KeyInfo;

static const KeyInfo KEYS[] = {
    {"task", 0, 2}, {"let", 1, 0}, {"ctx", 2, 0}, {"in", 2, 0},   {"err", 2, 0},
    {"for", 2, 1},  {"tone", 2, 1}, {"ask", 2, 0}, {"why", 2, 0}, {"ref", 2, 0},
    {"if", 2, 0},   {"out", 5, 2},
};
#define NKEYS (int)(sizeof KEYS / sizeof KEYS[0])

static char **custom;
static int ncustom;

void sema_add_custom_key(const char *name) {
    for (int i = 0; i < ncustom; i++)
        if (!strcmp(custom[i], name)) return;
    custom = xrealloc(custom, sizeof(char *) * (size_t)(ncustom + 1));
    custom[ncustom++] = xstrdup(name);
}

static const KeyInfo *key_info(const char *k) {
    for (int i = 0; i < NKEYS; i++)
        if (!strcmp(KEYS[i].name, k)) return &KEYS[i];
    return NULL;
}

static int is_custom(const char *k) {
    for (int i = 0; i < ncustom; i++)
        if (!strcmp(custom[i], k)) return 1;
    return 0;
}

int node_rank(const Node *n) {
    switch (n->kind) {
    case N_FIELD: { const KeyInfo *k = key_info(n->key); return k ? k->rank : 2; }
    case N_PLUS: case N_MINUS: case N_BANG: return 3;
    case N_STEP: return 4;
    }
    return 2;
}

static const char *suggest_key(const char *k) {
    const char *best = NULL;
    int bd = 3;
    for (int i = 0; i < NKEYS; i++) {
        int d = edit_distance(k, KEYS[i].name);
        if (d < bd && d < (int)strlen(k)) { bd = d; best = KEYS[i].name; }
    }
    for (int i = 0; i < ncustom; i++) {
        int d = edit_distance(k, custom[i]);
        if (d < bd && d < (int)strlen(k)) { bd = d; best = custom[i]; }
    }
    return best;
}

/* ---------- protected text: "quotes", `code`, ``` fences ``` ---------- */

static char *protect_mask(const char *s) {
    size_t n = strlen(s);
    char *m = xcalloc(n + 1, 1);
    size_t i = 0;
    while (i < n) {
        size_t end = 0;
        if (!strncmp(s + i, "```", 3)) {
            const char *e = strstr(s + i + 3, "```");
            end = e ? (size_t)(e - s) + 3 : n;
        } else if (s[i] == '`' || s[i] == '"') {
            const char *e = strchr(s + i + 1, s[i]);
            end = e ? (size_t)(e - s) + 1 : n;
        }
        if (end) {
            memset(m + i, 1, end - i);
            i = end;
        } else {
            i++;
        }
    }
    return m;
}

static int unprotected_contains(const char *s, const char *needle) {
    char *m = protect_mask(s);
    size_t l = strlen(needle);
    int found = 0;
    for (const char *p = strstr(s, needle); p && !found; p = strstr(p + 1, needle)) {
        size_t i = (size_t)(p - s), j;
        for (j = 0; j < l && !m[i + j]; j++) {}
        if (j == l) found = 1;
    }
    free(m);
    return found;
}

/* ---------- lint rules ---------- */

typedef struct {
    const char *from;
    const char *to;
    WarnId w;
    int opt;      /* optimizer level that applies the fix */
} Rule;

static const Rule RULES[] = {
    /* R1: filler (longest first) */
    {"i would like you to", "", W_FILLER, 1}, {"i'd like you to", "", W_FILLER, 1},
    {"i want you to", "", W_FILLER, 1},       {"can you please", "", W_FILLER, 1},
    {"could you please", "", W_FILLER, 1},    {"can you help me", "", W_FILLER, 1},
    {"could you help me", "", W_FILLER, 1},   {"would you mind", "", W_FILLER, 1},
    {"could you", "", W_FILLER, 1},           {"can you", "", W_FILLER, 1},
    {"would you", "", W_FILLER, 1},           {"please", "", W_FILLER, 1},
    {"kindly", "", W_FILLER, 1},              {"thank you", "", W_FILLER, 1},
    {"thanks", "", W_FILLER, 1},
    /* plain ASCII beats unicode symbols */
    {"\xE2\x86\x92", "->", W_UNICODE, 1}, /* → */
    {"\xE2\x87\x92", "->", W_UNICODE, 1}, /* ⇒ */
    {"\xE2\x89\xA4", "<=", W_UNICODE, 1}, /* ≤ */
    {"\xE2\x89\xA5", ">=", W_UNICODE, 1}, /* ≥ */
    {"\xE2\x89\xA0", "!=", W_UNICODE, 1}, /* ≠ */
    {"\xE2\x80\x94", "-", W_UNICODE, 1},  /* — */
    {"\xE2\x80\x93", "-", W_UNICODE, 1},  /* – */
    {"\xE2\x80\x9C", "\"", W_UNICODE, 1}, /* “ */
    {"\xE2\x80\x9D", "\"", W_UNICODE, 1}, /* ” */
    {"\xE2\x80\x98", "'", W_UNICODE, 1},  /* ‘ */
    {"\xE2\x80\x99", "'", W_UNICODE, 1},  /* ’ */
    {"\xE2\x80\xA6", "...", W_UNICODE, 1}, /* … */
    {"\xC3\x97", "x", W_UNICODE, 1},      /* × */
    /* R8: numeronyms cost more than the full word */
    {"k8s", "kubernetes", W_NUMERONYM, 1},       {"i18n", "internationalization", W_NUMERONYM, 1},
    {"a11y", "accessibility", W_NUMERONYM, 1},   {"l10n", "localization", W_NUMERONYM, 1},
    {"o11y", "observability", W_NUMERONYM, 1},
    /* §6: short forms that cost more */
    {"w/o", "without", W_ABBREV, 1}, {"w/", "with", W_ABBREV, 1}, {"e.g.", "like", W_ABBREV, 1},
    /* R10: digits */
    {"two", "2", W_NUMBER_WORDS, 1},      {"three", "3", W_NUMBER_WORDS, 1},
    {"four", "4", W_NUMBER_WORDS, 1},     {"five", "5", W_NUMBER_WORDS, 1},
    {"six", "6", W_NUMBER_WORDS, 1},      {"seven", "7", W_NUMBER_WORDS, 1},
    {"eight", "8", W_NUMBER_WORDS, 1},    {"nine", "9", W_NUMBER_WORDS, 1},
    {"ten", "10", W_NUMBER_WORDS, 1},     {"eleven", "11", W_NUMBER_WORDS, 1},
    {"twelve", "12", W_NUMBER_WORDS, 1},  {"fifteen", "15", W_NUMBER_WORDS, 1},
    {"twenty", "20", W_NUMBER_WORDS, 1},  {"one hundred", "100", W_NUMBER_WORDS, 1},
    {"a hundred", "100", W_NUMBER_WORDS, 1}, {"hundred", "100", W_NUMBER_WORDS, 1},
    /* R2: articles (aggressive, -O2) */
    {"the ", "", W_ARTICLES, 2}, {"an ", "", W_ARTICLES, 2}, {"a ", "", W_ARTICLES, 2},
};
#define NRULES (int)(sizeof RULES / sizeof RULES[0])

static int rule_matches(const Rule *r, const char *s, size_t i, const char *mask) {
    size_t l = strlen(r->from);
    if (!str_ieq_n(s + i, r->from, l) || strlen(s + i) < l) return 0;
    for (size_t j = 0; j < l; j++)
        if (mask[i + j]) return 0;
    if (is_word_char((unsigned char)r->from[0]) && i > 0 && is_word_char((unsigned char)s[i - 1])) return 0;
    if (is_word_char((unsigned char)r->from[l - 1]) && is_word_char((unsigned char)s[i + l])) return 0;
    return 1;
}

/* emoji and pictographs: 4-byte UTF-8, U+2600..U+27BF, plus joiners/variation selectors */
static size_t emoji_len(const unsigned char *p) {
    if (p[0] >= 0xF0 && p[0] <= 0xF4 && p[1] && p[2] && p[3]) return 4;
    if (p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E && p[2]) return 3;
    return 0;
}
static size_t joiner_len(const unsigned char *p) {
    if (p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F) return 3; /* VS16 */
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D) return 3; /* ZWJ  */
    return 0;
}

static void lint_warn(const Rule *r, Loc at, const char *orig) {
    switch (r->w) {
    case W_FILLER:
        warning_at(at, r->w, "'%s' is filler and carries no instruction (R1)", orig);
        break;
    case W_UNICODE:
        warning_at(at, r->w, "'%s' costs extra tokens; use ASCII '%s'", orig, r->to);
        break;
    case W_NUMERONYM:
        warning_at(at, r->w, "'%s' costs more tokens than '%s' (R8)", orig, r->to);
        break;
    case W_ABBREV:
        warning_at(at, r->w, "'%s' costs more tokens than '%s'", orig, r->to);
        break;
    case W_NUMBER_WORDS:
        warning_at(at, r->w, "write '%s' as the digit '%s' (R10)", orig, r->to);
        break;
    case W_ARTICLES:
        warning_at(at, r->w, "article '%.*s' can usually be dropped (R2)", (int)strlen(orig) - 1, orig);
        break;
    default:
        break;
    }
}

static char *cleanup(const char *s) {
    char *m = protect_mask(s);
    Buf b;
    buf_init(&b);
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (m[i]) { buf_putc(&b, c); continue; }
        char last = b.len ? b.data[b.len - 1] : '\0';
        if (c == ' ' || c == '\t') {
            if (b.len == 0 || last == ' ') continue;
            buf_putc(&b, ' ');
            continue;
        }
        if (strchr(",;:.)?!", c) && last == ' ' && b.len > 0 && !m[i - 1]) {
            b.len--;
            b.data[b.len] = '\0';
            last = b.len ? b.data[b.len - 1] : '\0';
        }
        if (c == ',' && (last == ',' || b.len == 0)) continue;
        buf_putc(&b, c);
    }
    free(m);
    char *r = buf_take(&b);
    size_t n = strlen(r);
    while (n > 0 && (r[n - 1] == ' ' || r[n - 1] == ',')) r[--n] = '\0';
    size_t k = 0;
    while (r[k] == ' ' || r[k] == ',') k++;
    if (k) memmove(r, r + k, n - k + 1);
    return r;
}

static char *lint_value(const char *v, Loc base, int opt) {
    char *mask = protect_mask(v);
    Buf b;
    buf_init(&b);
    int changed = 0;
    size_t n = strlen(v);
    for (size_t i = 0; i < n;) {
        if (mask[i]) { buf_putc(&b, v[i]); i++; continue; }
        const Rule *hit = NULL;
        for (int r = 0; r < NRULES; r++)
            if (rule_matches(&RULES[r], v, i, mask)) { hit = &RULES[r]; break; }
        Loc at = base;
        at.col = base.col ? base.col + (int)i : 0;
        if (hit) {
            size_t l = strlen(hit->from);
            char *orig = xstrndup(v + i, l);
            at.len = (int)l;
            lint_warn(hit, at, orig);
            free(orig);
            if (opt >= hit->opt) { buf_puts(&b, hit->to); changed = 1; }
            else buf_putn(&b, v + i, l);
            i += l;
            continue;
        }
        size_t el = emoji_len((const unsigned char *)v + i);
        if (el) {
            size_t tot = el, jl;
            while ((jl = joiner_len((const unsigned char *)v + i + tot)) ||
                   (jl = emoji_len((const unsigned char *)v + i + tot)))
                tot += jl;
            at.len = 1;
            warning_at(at, W_UNICODE, "emoji cost 2-4 tokens and carry no precise meaning; use a word");
            if (opt >= 1) changed = 1;
            else buf_putn(&b, v + i, tot);
            i += tot;
            continue;
        }
        buf_putc(&b, v[i]);
        i++;
    }
    free(mask);
    char *r = buf_take(&b);
    if (changed) {
        char *c = cleanup(r);
        free(r);
        r = c;
        if (!r[0]) { free(r); r = xstrdup(v); } /* never optimise a line away to nothing */
    }
    return r;
}

/* ---------- helpers ---------- */

static int contains_word(const char *hay, const char *w) {
    size_t l = strlen(w);
    for (const char *p = strstr(hay, w); p; p = strstr(p + 1, w)) {
        int before = p == hay || !is_ident_char((unsigned char)p[-1]);
        int after = !is_ident_char((unsigned char)p[l]);
        if (before && after) return 1;
    }
    return 0;
}

static const char *kind_text(const Node *n) {
    switch (n->kind) {
    case N_PLUS: return "+";
    case N_MINUS: return "-";
    case N_BANG: return "!";
    case N_STEP: return "step";
    default: return n->key;
    }
}

static int same_stmt(const Node *a, const Node *b) {
    if (a->kind != b->kind) return 0;
    if (a->kind == N_FIELD && strcmp(a->key, b->key)) return 0;
    if (a->kind == N_STEP) return 0;
    return !strcmp(a->value, b->value);
}

/* ---------- the pass ---------- */

void sema_run(Prompt *p, int opt) {
    int max_step = 0, have_task = 0;
    const Node *task = NULL, *out = NULL;

    /* 1. steps must be numbered 1, 2, 3, ... */
    int expect = 1;
    for (int i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->kind != N_STEP) continue;
        if (n->step != expect)
            error_at(n->loc, "step %d out of sequence; expected step %d", n->step, expect);
        expect = n->step + 1;
        if (n->step > max_step) max_step = n->step;
    }

    /* 2. statement-level checks */
    typedef struct { char *name; const Node *def; } Let;
    Let *lets = NULL;
    int nlets = 0;

    for (int i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (!n->value[0]) {
            Loc l = n->loc;
            error_at(l, "expected a value after '%s%s'", kind_text(n), n->kind == N_FIELD ? ":" : "");
            continue;
        }
        if (n->kind != N_FIELD) continue;

        const KeyInfo *ki = key_info(n->key);
        if (!ki && !is_custom(n->key)) {
            const char *s = suggest_key(n->key);
            if (s && !strcmp(s, "task")) have_task = 1; /* don't cascade into 'no task' */
            if (s) error_at(n->loc, "unknown key '%s'; did you mean '%s'?", n->key, s);
            else error_at(n->loc, "unknown key '%s' (declare custom keys with '#pragma key %s')", n->key, n->key);
            continue;
        }
        if (!strcmp(n->key, "task")) { have_task = 1; }
        if (ki && ki->single) {
            for (int j = 0; j < i; j++) {
                Node *m = &p->v[j];
                if (m->kind == N_FIELD && !strcmp(m->key, n->key)) {
                    if (ki->single == 2) {
                        error_at(n->loc, "redefinition of '%s:'", n->key);
                        note_at(m->loc, "previous definition is here");
                    } else {
                        warning_at(n->loc, W_REPEAT_KEY, "'%s:' given more than once; the model sees both", n->key);
                        note_at(m->loc, "first given here");
                    }
                    break;
                }
            }
        }
        if (!strcmp(n->key, "task") && !task) task = n;
        if (!strcmp(n->key, "out") && !out) out = n;

        if (!strcmp(n->key, "let")) {
            const char *eq = strchr(n->value, '=');
            char *name = eq ? xstrndup(n->value, (size_t)(eq - n->value)) : NULL;
            char *tn = name ? xtrim(name) : NULL;
            free(name);
            int ok = tn && tn[0] && is_ident_start((unsigned char)tn[0]);
            for (size_t k = 0; ok && tn[k]; k++)
                if (!is_ident_char((unsigned char)tn[k]) && tn[k] != '-') ok = 0;
            char *rhs = eq ? xtrim(eq + 1) : NULL;
            if (!ok || !rhs || !rhs[0]) {
                Loc l = n->loc;
                l.col = n->vcol;
                l.len = (int)strlen(n->value);
                error_at(l, "expected 'name = value' in 'let:'");
            } else {
                int dup = 0;
                for (int k = 0; k < nlets; k++)
                    if (!strcmp(lets[k].name, tn)) {
                        Loc l = n->loc;
                        l.col = n->vcol;
                        l.len = (int)strlen(tn);
                        error_at(l, "redefinition of '%s'", tn);
                        note_at(lets[k].def->loc, "previous definition is here");
                        dup = 1;
                    }
                if (!dup) {
                    lets = xrealloc(lets, sizeof(Let) * (size_t)(nlets + 1));
                    lets[nlets++] = (Let){xstrdup(tn), n};
                }
            }
            free(tn);
            free(rhs);
        } else if (!strcmp(n->key, "if")) {
            if (!unprotected_contains(n->value, "->")) {
                Loc l = n->loc;
                l.col = n->vcol;
                l.len = (int)strlen(n->value);
                error_at(l, "expected '->' in 'if:' (write it as 'condition -> action')");
            }
        } else if (!strcmp(n->key, "err")) {
            char c = n->value[0];
            if (c != '"' && c != '\'' && c != '`') {
                Loc l = n->loc;
                l.col = n->vcol;
                l.len = (int)strlen(n->value);
                warning_at(l, W_ERR_QUOTE, "error text is not quoted; paste it exactly and wrap it in quotes (R11)");
            }
        }
    }

    /* 3. references to steps */
    for (int i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        int is_ref = n->kind == N_FIELD && !strcmp(n->key, "ref");
        if (!is_ref && max_step == 0) continue;
        char *mask = protect_mask(n->value);
        for (const char *s = n->value; *s; s++) {
            size_t off = (size_t)(s - n->value);
            if (mask[off] || !str_ieq_n(s, "step ", 5)) continue;
            if (off > 0 && is_word_char((unsigned char)s[-1])) continue;
            if (!isdigit((unsigned char)s[5])) continue;
            int k = atoi(s + 5);
            if (k < 1 || k > max_step) {
                Loc l = n->loc;
                l.col = n->vcol + (int)off;
                l.len = 5;
                while (isdigit((unsigned char)s[l.len])) l.len++;
                if (max_step) error_at(l, "reference to step %d, but the prompt has only %d step%s", k, max_step,
                                       max_step == 1 ? "" : "s");
                else error_at(l, "reference to step %d, but the prompt has no numbered steps", k);
            } else if (n->kind == N_STEP && k >= n->step) {
                Loc l = n->loc;
                l.col = n->vcol + (int)off;
                l.len = 6;
                error_at(l, "step %d refers to step %d, which has not run yet", n->step, k);
            }
        }
        free(mask);
    }

    /* 4. lint + optimise every value (err: text is never touched) */
    for (int i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->kind == N_FIELD && !strcmp(n->key, "err")) continue;
        Loc base = n->loc;
        base.col = n->subst ? 0 : n->vcol;
        char *nv = lint_value(n->value, base, opt);
        free(n->value);
        n->value = nv;
    }

    /* 5. duplicates (R13) */
    for (int i = 0; i < p->n; i++)
        for (int j = 0; j < i; j++)
            if (!p->v[j].dead && same_stmt(&p->v[i], &p->v[j])) {
                warning_at(p->v[i].loc, W_DUPLICATE, "duplicate line; it repeats an earlier line (R13)");
                note_at(p->v[j].loc, "first written here");
                if (opt >= 1) p->v[i].dead = 1;
                break;
            }

    /* 6. unused let: names */
    for (int k = 0; k < nlets; k++) {
        int used = 0;
        for (int i = 0; i < p->n && !used; i++) {
            const Node *n = &p->v[i];
            if (n == lets[k].def) continue;
            if (contains_word(n->value, lets[k].name)) used = 1;
        }
        if (!used) {
            Loc l = lets[k].def->loc;
            l.col = lets[k].def->vcol;
            l.len = (int)strlen(lets[k].name);
            warning_at(l, W_UNUSED_LET, "unused name '%s' (R15: use let: only for things you repeat)", lets[k].name);
        }
        free(lets[k].name);
    }
    free(lets);

    /* 7. whole-prompt checks */
    if (p->n == 0) {
        error_at((Loc){p->file, 0, 0, 0, NULL}, "empty prompt");
        return;
    }
    if (!have_task && max_step == 0) {
        Loc l = p->v[0].loc;
        error_at(l, "prompt has no 'task:' and no numbered steps; every prompt needs one (like main() in C)");
    }
    if (!out) {
        Loc l = task ? task->loc : p->v[0].loc;
        l.col = 0;
        warning_at(l, W_NO_OUT, "prompt has no 'out:' line; the model will pick its own format and length (R17)");
    } else {
        int has_num = 0;
        for (const char *s = out->value; *s; s++)
            if (isdigit((unsigned char)*s)) has_num = 1;
        static const char *KW[] = {"terse", "code", "y/n", "json", "changed lines", "1 line", "diff"};
        for (size_t k = 0; k < sizeof KW / sizeof KW[0]; k++)
            if (strstr(out->value, KW[k])) has_num = 1;
        if (!has_num) {
            Loc l = out->loc;
            l.col = out->vcol;
            l.len = (int)strlen(out->value);
            warning_at(l, W_OUT_LENGTH, "'out:' sets no length; add a number like 'max 100 words' (R18)");
        }
    }
    if (task && task != &p->v[0] && opt < 2)
        warning_at(task->loc, W_FIELD_ORDER, "'task:' should be the first line (-O2 reorders)");
    if (out) {
        int last = p->n - 1;
        while (last > 0 && p->v[last].dead) last--;
        if (out != &p->v[last] && opt < 2)
            warning_at(out->loc, W_FIELD_ORDER, "'out:' should be the last line (-O2 reorders)");
    }

    /* 8. -O2: canonical order task -> let -> context -> + - ! -> steps -> out (stable) */
    if (opt >= 2 && diag_errors() == 0) {
        for (int i = 1; i < p->n; i++) {
            Node key = p->v[i];
            int r = node_rank(&key), j = i - 1;
            while (j >= 0 && node_rank(&p->v[j]) > r) { p->v[j + 1] = p->v[j]; j--; }
            p->v[j + 1] = key;
        }
    }
}
