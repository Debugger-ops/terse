/* emit.c — code generation.
 *   terse     canonical, compact Terse text (default)
 *   json     structured object for programs and APIs
 *   english  plain-English expansion, for models or teammates that don't know Terse
 */
#include "tersec.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *TERSE_PRIMER =
    "You will receive prompts in Terse, a compact prompt format. Read them as follows.\n"
    "Lines are `key: value`. Keys: task (action), ctx (background), in (input), err (exact error), "
    "for (audience), tone (voice), ask (request inside a drafted message), why (reason), out (answer "
    "format and length), let (define a name to reuse), if (condition), ref (point to earlier part).\n"
    "Operators: `+` include, `-` exclude, `!` hard rule never broken, `->` becomes/then, `|` or, `?` you "
    "decide, `1. 2.` ordered steps, `=` defines, `~` approximately.\n"
    "Follow `out:` exactly. `out: terse` means no intro, no summary, no filler. `out: terse syntax` means reply "
    "in Terse format too.\n"
    "If a Terse prompt is ambiguous, ask one short question instead of guessing.\n";

/* ---------- terse ---------- */

static void emit_terse(const Prompt *p, Buf *b) {
    for (int i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->dead) continue;
        switch (n->kind) {
        case N_FIELD: buf_printf(b, "%s: %s\n", n->key, n->value); break;
        case N_PLUS: buf_printf(b, "+ %s\n", n->value); break;
        case N_MINUS: buf_printf(b, "- %s\n", n->value); break;
        case N_BANG: buf_printf(b, "! %s\n", n->value); break;
        case N_STEP: buf_printf(b, "%d. %s\n", n->step, n->value); break;
        }
    }
}

/* ---------- json ---------- */

static void json_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

static void json_list(Buf *b, const Prompt *p, NodeKind k, const char *name, int *first) {
    int any = 0;
    for (int i = 0; i < p->n; i++)
        if (!p->v[i].dead && p->v[i].kind == k) any = 1;
    if (!any) return;
    buf_printf(b, "%s\n  \"%s\": [", *first ? "" : ",", name);
    *first = 0;
    int f = 1;
    for (int i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->dead || n->kind != k) continue;
        buf_puts(b, f ? "\n    " : ",\n    ");
        f = 0;
        json_str(b, n->value);
    }
    buf_puts(b, "\n  ]");
}

static void emit_json(const Prompt *p, Buf *b) {
    buf_puts(b, "{");
    int first = 1;
    /* fields in order of first appearance; repeated keys are joined with newlines */
    for (int i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->dead || n->kind != N_FIELD || !strcmp(n->key, "let")) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (!p->v[j].dead && p->v[j].kind == N_FIELD && !strcmp(p->v[j].key, n->key)) seen = 1;
        if (seen) continue;
        Buf v;
        buf_init(&v);
        for (int j = i; j < p->n; j++)
            if (!p->v[j].dead && p->v[j].kind == N_FIELD && !strcmp(p->v[j].key, n->key)) {
                if (v.len) buf_putc(&v, '\n');
                buf_puts(&v, p->v[j].value);
            }
        buf_printf(b, "%s\n  ", first ? "" : ",");
        first = 0;
        json_str(b, n->key);
        buf_puts(b, ": ");
        json_str(b, v.data);
        free(v.data);
    }
    int any_let = 0;
    for (int i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->dead || n->kind != N_FIELD || strcmp(n->key, "let")) continue;
        const char *eq = strchr(n->value, '=');
        if (!eq) continue;
        char *nm = xstrndup(n->value, (size_t)(eq - n->value));
        char *tn = xtrim(nm);
        char *tv = xtrim(eq + 1);
        if (!any_let) { buf_printf(b, "%s\n  \"let\": {", first ? "" : ","); first = 0; }
        buf_puts(b, any_let ? ",\n    " : "\n    ");
        any_let = 1;
        json_str(b, tn);
        buf_puts(b, ": ");
        json_str(b, tv);
        free(nm); free(tn); free(tv);
    }
    if (any_let) buf_puts(b, "\n  }");
    json_list(b, p, N_PLUS, "include", &first);
    json_list(b, p, N_MINUS, "exclude", &first);
    json_list(b, p, N_BANG, "rules", &first);
    json_list(b, p, N_STEP, "steps", &first);
    buf_puts(b, "\n}\n");
}

/* ---------- english ---------- */

static void sentence(Buf *b, const char *prefix, const char *v) {
    buf_puts(b, prefix);
    if (strchr(v, '\n')) { buf_printf(b, "\n%s\n", v); return; }
    buf_puts(b, v);
    size_t n = strlen(v);
    char last = n ? v[n - 1] : '.';
    if (!strchr(".!?\"'`)", last)) buf_putc(b, '.');
    buf_putc(b, '\n');
}

static void joined(Buf *b, const Prompt *p, NodeKind k, const char *prefix) {
    Buf v;
    buf_init(&v);
    for (int i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->dead || n->kind != k) continue;
        if (v.len) buf_puts(&v, "; ");
        buf_puts(&v, n->value);
    }
    if (v.len) sentence(b, prefix, v.data);
    free(v.data);
}

static void emit_english(const Prompt *p, Buf *b) {
    for (int rank = 0; rank <= 5; rank++) {
        if (rank == 3) {
            joined(b, p, N_PLUS, "Make sure to include: ");
            joined(b, p, N_MINUS, "Do not include: ");
            for (int i = 0; i < p->n; i++)
                if (!p->v[i].dead && p->v[i].kind == N_BANG)
                    sentence(b, "Hard rule, never break it: ", p->v[i].value);
            continue;
        }
        if (rank == 4) {
            int any = 0;
            for (int i = 0; i < p->n; i++) {
                const Node *n = &p->v[i];
                if (n->dead || n->kind != N_STEP) continue;
                if (!any) buf_puts(b, "Do these steps in order:\n");
                any = 1;
                buf_printf(b, "%d. %s\n", n->step, n->value);
            }
            continue;
        }
        for (int i = 0; i < p->n; i++) {
            const Node *n = &p->v[i];
            if (n->dead || n->kind != N_FIELD || node_rank(n) != rank) continue;
            const char *k = n->key, *v = n->value;
            if (!strcmp(k, "task")) {
                char *c = xstrdup(v);
                c[0] = (char)toupper((unsigned char)c[0]);
                sentence(b, "", c);
                free(c);
            } else if (!strcmp(k, "ctx")) sentence(b, "Context: ", v);
            else if (!strcmp(k, "in")) sentence(b, "Input: ", v);
            else if (!strcmp(k, "err")) { buf_printf(b, "The exact error message is: %s\n", v); }
            else if (!strcmp(k, "for")) sentence(b, "Write it for this audience: ", v);
            else if (!strcmp(k, "tone")) sentence(b, "Use this tone: ", v);
            else if (!strcmp(k, "ask")) sentence(b, "The message should ask for: ", v);
            else if (!strcmp(k, "why")) sentence(b, "The reason is: ", v);
            else if (!strcmp(k, "ref")) sentence(b, "Refer back to ", v);
            else if (!strcmp(k, "out")) sentence(b, "Format the answer as: ", v);
            else if (!strcmp(k, "let")) {
                const char *eq = strchr(v, '=');
                if (eq) {
                    char *nm = xstrndup(v, (size_t)(eq - v));
                    char *tn = xtrim(nm), *tv = xtrim(eq + 1);
                    Buf s;
                    buf_init(&s);
                    buf_printf(&s, "\"%s\" means: ", tn);
                    sentence(b, s.data, tv);
                    free(s.data); free(nm); free(tn); free(tv);
                }
            } else if (!strcmp(k, "if")) {
                const char *arrow = strstr(v, "->");
                if (arrow) {
                    char *cond = xstrndup(v, (size_t)(arrow - v));
                    char *tc = xtrim(cond), *ta = xtrim(arrow + 2);
                    Buf s;
                    buf_init(&s);
                    buf_printf(&s, "If %s, then ", tc);
                    sentence(b, s.data, ta);
                    free(s.data); free(cond); free(tc); free(ta);
                } else sentence(b, "If: ", v);
            } else {
                Buf s;
                buf_init(&s);
                buf_printf(&s, "%s: ", k);
                s.data[0] = (char)toupper((unsigned char)s.data[0]);
                sentence(b, s.data, v);
                free(s.data);
            }
        }
    }
}

char *emit(const Prompt *p, EmitKind kind, int with_primer) {
    Buf b;
    buf_init(&b);
    if (with_primer && kind == EMIT_TERSE) {
        buf_puts(&b, TERSE_PRIMER);
        buf_puts(&b, "\n");
    }
    switch (kind) {
    case EMIT_TERSE: emit_terse(p, &b); break;
    case EMIT_JSON: emit_json(p, &b); break;
    case EMIT_ENGLISH: emit_english(p, &b); break;
    }
    return buf_take(&b);
}

/* Rough token estimate (no real tokenizer is bundled):
 * short words ~1 token, long words ~1 per 6 letters, digits ~1 per 3,
 * each punctuation run ~1 per 2 chars, each newline 1, each non-ASCII char ~1. */
int approx_tokens(const char *s) {
    int t = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (isalpha(*p)) {
            int n = 0;
            while (isalpha(*p)) { n++; p++; }
            t += n <= 8 ? 1 : (n + 5) / 6;
        } else if (isdigit(*p)) {
            int n = 0;
            while (isdigit(*p)) { n++; p++; }
            t += (n + 2) / 3;
        } else if (*p == '\n') {
            t++; p++;
        } else if (*p == ' ' || *p == '\t') {
            p++;
        } else if (*p >= 0x80) {
            t++; p++;
            while ((*p & 0xC0) == 0x80) p++;
        } else {
            int n = 0;
            while (*p && ispunct(*p)) { n++; p++; }
            t += (n + 1) / 2;
        }
    }
    return t;
}
