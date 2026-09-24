#define _POSIX_C_SOURCE 200809L
/* main.c — the tersec command line driver. */
#include "tersec.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f) {
    fprintf(f,
        "usage: terse <command> [options] file.terse...\n"
        "       tersec [options] file.terse...\n"
        "\n"
        "Compile Terse source into a prompt for an AI model.\n"
        "\n"
        "Commands:\n"
        "  build                  compile and print the prompt (same as plain tersec)\n"
        "  check                  check for errors and warnings, print nothing\n"
        "  stats                  print approximate token counts only\n"
        "  help                   print this help\n"
        "  version                print version\n"
        "\n"
        "Output:\n"
        "  -o <file>              write output to <file> (default: stdout)\n"
        "  --emit=terse            compact Terse prompt (default)\n"
        "  --emit=json            structured JSON\n"
        "  --emit=english         plain-English expansion\n"
        "  --primer               prepend the Terse primer (for models that haven't seen Terse)\n"
        "  -E                     run the preprocessor only\n"
        "  -fsyntax-only          check the source, write nothing\n"
        "  --stats                print approximate token counts to stderr\n"
        "\n"
        "Optimization:\n"
        "  -O0                    no rewrites\n"
        "  -O1                    safe rewrites: filler, unicode, numeronyms, number words, duplicates (default)\n"
        "  -O2                    also drop articles and reorder lines canonically (review the result)\n"
        "\n"
        "Preprocessor:\n"
        "  -D NAME[=value]        define NAME (default value 1)\n"
        "  -U NAME                undefine NAME\n"
        "  -I <dir>               add <dir> to the #include search path\n"
        "\n"
        "Diagnostics:\n"
        "  -Wall                  default warnings (on unless -w)\n"
        "  -Wextra                also -Wfield-order, -Warticles and -Wout-length\n"
        "  -W<name>, -Wno-<name>  turn one warning on or off\n"
        "  -Werror                treat warnings as errors\n"
        "  -w                     silence all warnings\n"
        "  --color=auto|always|never\n"
        "\n"
        "  --version              print version\n"
        "  -h, --help             print this help\n"
        "\n"
        "Warning names: filler unicode numeronym abbrev number-words missing-out duplicate unused-let\n"
        "               err-quote field-order repeat-key redefined user unknown-pragma articles out-length\n");
}

static void define_arg(const char *a) {
    const char *eq = strchr(a, '=');
    if (eq) {
        char *n = xstrndup(a, (size_t)(eq - a));
        pp_define(n, eq + 1);
        free(n);
    } else {
        pp_define(a, "1");
    }
}

/* `terse build x.terse` style: rewrite the subcommand into the equivalent tersec flag. */
static int no_output = 0;

static int apply_subcommand(int *argc, char **argv) {
    if (*argc < 2) return 0;
    const char *c = argv[1];
    const char *flag = NULL;
    if (!strcmp(c, "build")) flag = "";
    else if (!strcmp(c, "check")) flag = "-fsyntax-only";
    else if (!strcmp(c, "stats")) { flag = "--stats"; no_output = 1; }
    else if (!strcmp(c, "help")) flag = "--help";
    else if (!strcmp(c, "version")) flag = "--version";
    else return 0;
    if (flag[0]) argv[1] = (char *)flag;
    else { for (int i = 1; i < *argc; i++) argv[i] = argv[i + 1]; (*argc)--; }
    return 1;
}

int main(int argc, char **argv) {
    apply_subcommand(&argc, argv);
    const char *outpath = NULL;
    const char **inputs = xmalloc(sizeof(char *) * (size_t)(argc + 1));
    int ninputs = 0;
    int opt = 1, only_pp = 0, syntax_only = 0, primer = 0, stats = 0;
    int color = isatty(2);
    EmitKind kind = EMIT_TERSE;

    diag_init();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT_ARG(dst)                                                          \
    do {                                                                       \
        if (a[2]) dst = a + 2;                                                 \
        else if (i + 1 < argc) dst = argv[++i];                                \
        else { fprintf(stderr, "tersec: error: missing argument to '%s'\n", a); return 2; } \
    } while (0)
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--version")) {
            printf("tersec %s (Terse language %s)\n", TERSEC_VERSION, TERSE_LANG_VERSION);
            return 0;
        }
        else if (!strcmp(a, "-o")) {
            if (i + 1 >= argc) { fprintf(stderr, "tersec: error: missing filename after '-o'\n"); return 2; }
            outpath = argv[++i];
        }
        else if (!strncmp(a, "-D", 2)) { const char *v; NEXT_ARG(v); define_arg(v); }
        else if (!strncmp(a, "-U", 2)) { const char *v; NEXT_ARG(v); pp_undef(v); }
        else if (!strncmp(a, "-I", 2)) { const char *v; NEXT_ARG(v); pp_add_include_dir(v); }
        else if (!strcmp(a, "-O0")) opt = 0;
        else if (!strcmp(a, "-O1") || !strcmp(a, "-O")) opt = 1;
        else if (!strcmp(a, "-O2") || !strcmp(a, "-O3")) opt = 2;
        else if (!strcmp(a, "-E")) only_pp = 1;
        else if (!strcmp(a, "-fsyntax-only")) syntax_only = 1;
        else if (!strcmp(a, "--primer")) primer = 1;
        else if (!strcmp(a, "--stats")) stats = 1;
        else if (!strcmp(a, "--emit=terse")) kind = EMIT_TERSE;
        else if (!strcmp(a, "--emit=json")) kind = EMIT_JSON;
        else if (!strcmp(a, "--emit=english")) kind = EMIT_ENGLISH;
        else if (!strncmp(a, "--emit=", 7)) {
            fprintf(stderr, "tersec: error: unknown --emit target '%s' (use terse, json or english)\n", a + 7);
            return 2;
        }
        else if (!strcmp(a, "--color=always") || !strcmp(a, "-fcolor-diagnostics")) color = 1;
        else if (!strcmp(a, "--color=never") || !strcmp(a, "-fno-color-diagnostics")) color = 0;
        else if (!strcmp(a, "--color=auto")) color = isatty(2);
        else if (diag_option(a)) {}
        else if (!strcmp(a, "-")) inputs[ninputs++] = a;
        else if (a[0] == '-') {
            fprintf(stderr, "tersec: error: unknown option '%s' (see tersec --help)\n", a);
            return 2;
        }
        else inputs[ninputs++] = a;
#undef NEXT_ARG
    }
    diag_set_color(color && !getenv("NO_COLOR"));

    if (ninputs == 0) {
        if (argc <= 1) { usage(stderr); return 2; }
        fprintf(stderr, "tersec: fatal error: no input files\n");
        return 2;
    }
    if (outpath && ninputs > 1 && !syntax_only) {
        fprintf(stderr, "tersec: fatal error: cannot use -o with multiple input files\n");
        return 2;
    }

    Buf result;
    buf_init(&result);
    int any_error = 0;

    for (int f = 0; f < ninputs; f++) {
        int errs_before = diag_errors();
        PLines *pl = pp_run(inputs[f]);
        const char *shown = strcmp(inputs[f], "-") ? inputs[f] : "<stdin>";

        if (only_pp) {
            for (int i = 0; i < pl->n; i++) {
                buf_puts(&result, pl->v[i].text);
                buf_putc(&result, '\n');
            }
            continue;
        }

        Prompt *p = parse(pl, shown);
        sema_run(p, opt);
        if (diag_errors() > errs_before) { any_error = 1; continue; }
        if (syntax_only) continue;

        char *text = emit(p, kind, primer);
        if (ninputs > 1 && result.len) buf_puts(&result, "---\n");
        buf_puts(&result, text);

        if (stats) {
            Buf src;
            buf_init(&src);
            for (int i = 0; i < pl->n; i++) { buf_puts(&src, pl->v[i].text); buf_putc(&src, '\n'); }
            char *eng = emit(p, EMIT_ENGLISH, 0);
            char *terse = emit(p, EMIT_TERSE, 0);
            int ts = approx_tokens(src.data ? src.data : ""), to = approx_tokens(text);
            int te = approx_tokens(eng), tc = approx_tokens(terse);
            fprintf(stderr, "%s: approximate tokens (heuristic estimate, not a real tokenizer)\n", shown);
            fprintf(stderr, "  source, preprocessed      %5d\n", ts);
            fprintf(stderr, "  output                    %5d\n", to);
            fprintf(stderr, "  plain-English equivalent  %5d\n", te);
            if (te > 0)
                fprintf(stderr, "  Terse vs English           %4d%% smaller\n", (int)(100.0 * (te - tc) / te + 0.5));
            free(src.data); free(eng); free(terse);
        }
        free(text);
    }

    diag_summary();

    if (any_error || diag_errors()) { free(result.data); return 1; }
    if (syntax_only || no_output) { free(result.data); return 0; }

    if (outpath) {
        FILE *fo = fopen(outpath, "wb");
        if (!fo) { fprintf(stderr, "tersec: fatal error: cannot write '%s'\n", outpath); return 2; }
        if (result.len) fwrite(result.data, 1, result.len, fo);
        fclose(fo);
    } else if (result.len) {
        fwrite(result.data, 1, result.len, stdout);
    }
    free(result.data);
    free(inputs);
    return 0;
}
