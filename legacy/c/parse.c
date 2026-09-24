/* parse.c — turns preprocessed lines into a Prompt (a list of statements).
 *
 * Grammar (one statement per line):
 *   statement := field | include | exclude | rule | step
 *   field     := KEY ':' value            e.g.  task: summarize article
 *   include   := '+' ' ' value            e.g.  + type hints
 *   exclude   := '-' ' ' value            e.g.  - praise
 *   rule      := '!' ' ' value            e.g.  ! no invented facts
 *   step      := DIGITS '.' ' ' value     e.g.  2. count missing values
 *   value     := text [ ``` code fence spanning following lines ``` ]
 */
#include "tersec.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void push(Prompt *p, Node n) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 32;
        p->v = xrealloc(p->v, sizeof(Node) * (size_t)p->cap);
    }
    p->v[p->n++] = n;
}

Prompt *parse(PLines *pl, const char *file) {
    Prompt *pr = xcalloc(1, sizeof(Prompt));
    pr->file = file;
    for (int i = 0; i < pl->n; i++) {
        PLine *L = &pl->v[i];
        if (L->raw) continue; /* orphan fence content; already reported by the preprocessor */
        const char *s = L->text;
        const char *p = s;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        Node n;
        memset(&n, 0, sizeof n);
        n.loc = L->loc;
        n.subst = L->subst;
        n.loc.col = (int)(p - s) + 1;
        n.loc.len = 1;
        const char *v = NULL;

        if (isdigit((unsigned char)*p)) {
            const char *q = p;
            while (isdigit((unsigned char)*q)) q++;
            if (*q == '.' && (q[1] == ' ' || q[1] == '\t' || q[1] == '\0')) {
                n.kind = N_STEP;
                n.step = atoi(p);
                n.loc.len = (int)(q - p + 1);
                v = q + 1;
            }
        }
        if (!v && (*p == '+' || *p == '-' || *p == '!') && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
            n.kind = *p == '+' ? N_PLUS : *p == '-' ? N_MINUS : N_BANG;
            v = p + 1;
        }
        if (!v && (isalpha((unsigned char)*p) || *p == '_')) {
            const char *q = p;
            while (isalnum((unsigned char)*q) || *q == '_' || *q == '-') q++;
            if (*q == ':') {
                n.kind = N_FIELD;
                n.key = xstrndup(p, (size_t)(q - p));
                n.loc.len = (int)(q - p + 1);
                v = q + 1;
            }
        }
        if (!v) {
            Loc l = n.loc;
            const char *e = p;
            while (*e && *e != ' ') e++;
            l.len = (int)(e - p);
            error_at(l, "expected 'key:', '+', '-', '!' or a step number like '1.' at start of line");
            continue;
        }
        while (*v == ' ' || *v == '\t') v++;
        n.vcol = (int)(v - s) + 1;

        /* trim trailing whitespace */
        size_t vl = strlen(v);
        while (vl > 0 && isspace((unsigned char)v[vl - 1])) vl--;
        Buf b;
        buf_init(&b);
        buf_putn(&b, v, vl);

        /* a value that opens a ``` fence swallows the raw lines that follow */
        if (b.data && count_substr(b.data, "```") % 2 == 1) {
            while (i + 1 < pl->n && pl->v[i + 1].raw) {
                i++;
                buf_putc(&b, '\n');
                buf_puts(&b, pl->v[i].text);
                if (count_substr(pl->v[i].text, "```") % 2 == 1) break;
            }
        }
        n.value = buf_take(&b);
        push(pr, n);
    }
    return pr;
}
