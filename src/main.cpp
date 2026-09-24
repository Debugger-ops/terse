// main.cpp — the tersec command line driver.
#include "tersec.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace terse {
namespace {

void usage(std::FILE *f) {
    std::fputs(
        "usage: terse <command> [options] file.terse...\n"
        "       tersec [options] file.terse...\n"
        "\n"
        "Compile Terse source into a prompt for an AI model.\n"
        "\n"
        "Commands:\n"
        "  build                  compile and print the prompt (same as plain tersec)\n"
        "  check                  check for errors and warnings, print nothing\n"
        "  stats                  print approximate token counts only\n"
        "  fmt                    reformat source to the canonical layout (same as --format)\n"
        "  fix                    write the optimizer's rewrites back into the source (same as --fix)\n"
        "  help                   print this help\n"
        "  version                print version\n"
        "\n"
        "Output:\n"
        "  -o <file>              write output to <file> (default: stdout)\n"
        "  --emit=terse           compact Terse prompt (default)\n"
        "  --emit=json            structured JSON\n"
        "  --emit=english         plain-English expansion\n"
        "  --emit=xml             XML-style tags (<task>, <context>, <rules>...)\n"
        "  --emit=markdown        bold labels and lists (also --emit=md)\n"
        "  --emit=yaml            same structure as json, as YAML\n"
        "  --primer               prepend the Terse primer (for models that haven't seen Terse)\n"
        "  -E                     run the preprocessor only\n"
        "  -fsyntax-only          check the source, write nothing\n"
        "  --stats                print approximate token counts to stderr\n"
        "  --budget=N             error if the output is over ~N tokens (overrides #pragma budget)\n"
        "\n"
        "Source rewriting:\n"
        "  --format               print the file in canonical layout\n"
        "  --format -i            ...rewrite the file in place\n"
        "  --format --check       exit 1 if any file is not formatted (for CI)\n"
        "  --fix                  apply the optimizer's rewrites to the source file (uses the -O level)\n"
        "  --fix --dry-run        print them as a diff instead\n"
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
        "  --diagnostics-format=text|json   json: one JSON array on stderr, for editors\n"
        "\n"
        "  --version              print version\n"
        "  -h, --help             print this help\n"
        "\n"
        "Warning names: filler unicode numeronym abbrev number-words missing-out duplicate unused-let\n"
        "               err-quote field-order repeat-key redefined user unknown-pragma articles out-length\n",
        f);
}

// `terse build x.terse` style: rewrite the subcommand into the equivalent tersec flag.
void apply_subcommand(std::vector<std::string> &args, bool &no_output) {
    if (args.size() < 2) return;
    const std::string &c = args[1];
    if (c == "build") args.erase(args.begin() + 1);
    else if (c == "check") args[1] = "-fsyntax-only";
    else if (c == "stats") { args[1] = "--stats"; no_output = true; }
    else if (c == "help") args[1] = "--help";
    else if (c == "version") args[1] = "--version";
    else if (c == "fmt" || c == "format") args[1] = "--format";
    else if (c == "fix") args[1] = "--fix";
}

std::string join_lines(const PLines &pl) {
    std::string s;
    for (const PLine &l : pl) {
        s += l.text;
        s += '\n';
    }
    return s;
}

bool write_output(const std::string &path, const std::string &data) {
    std::FILE *fo = std::fopen(path.c_str(), "wb");
    if (!fo) return false;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), fo);
    std::fclose(fo);
    return true;
}

// `terse fmt`: no compile, just layout.
int run_format(const std::vector<std::string> &inputs, bool in_place, bool check) {
    int rc = 0;
    std::string out;
    for (const std::string &input : inputs) {
        std::optional<std::string> raw = read_file(input);
        if (!raw) throw FatalError(cat(input, ": No such file or directory"));
        std::string formatted = format_source(*raw);
        if (check) {
            if (formatted != *raw) {
                std::fprintf(stderr, "%s: not formatted (run 'terse fmt -i %s')\n", input.c_str(), input.c_str());
                rc = 1;
            }
        } else if (in_place && input != "-") {
            if (formatted != *raw && !write_file(input, formatted))
                throw FatalError(cat("cannot write '", input, "'"));
        } else {
            out += formatted;
        }
    }
    if (!out.empty()) std::fwrite(out.data(), 1, out.size(), stdout);
    return rc;
}

// Over budget: an error, plus notes on the three most expensive lines.
void report_budget(Diagnostics &diag, const Prompt &p, int tokens, int budget, const Loc &where) {
    diag.error(where, cat("prompt is ~", tokens, " tokens, over the budget of ", budget, " (by ~", tokens - budget, ")"));
    std::vector<std::pair<int, const Node *>> cost;
    for (const Node &n : p.nodes)
        if (!n.dead) cost.emplace_back(approx_tokens(terse_line(n)), &n);
    std::stable_sort(cost.begin(), cost.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (std::size_t i = 0; i < cost.size() && i < 3; i++)
        diag.note(cost[i].second->loc, cat("this line costs ~", cost[i].first, " tokens"));
}

int run(std::vector<std::string> args, Diagnostics &diag) {
    bool no_output = false;
    apply_subcommand(args, no_output);

    KeyRegistry keys;
    Preprocessor pp(diag, keys);
    Sema sema(diag, keys);

    std::optional<std::string> outpath;
    std::vector<std::string> inputs;
    int opt = 1;
    bool only_pp = false, syntax_only = false, primer = false, stats = false;
    bool format = false, in_place = false, check = false, fix = false, dry_run = false;
    std::optional<int> cli_budget;
    bool color = isatty(2);
    EmitKind kind = EmitKind::Terse;

    for (std::size_t i = 1; i < args.size(); i++) {
        const std::string &a = args[i];
        // value glued on (-DX) or in the next argument (-D X)
        auto next_arg = [&](std::string &dst) {
            if (a.size() > 2) { dst = a.substr(2); return true; }
            if (i + 1 < args.size()) { dst = args[++i]; return true; }
            std::fprintf(stderr, "tersec: error: missing argument to '%s'\n", a.c_str());
            return false;
        };
        auto starts = [&](std::string_view prefix) { return a.compare(0, prefix.size(), prefix) == 0; };
        std::string v;

        if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a == "--version") {
            std::printf("tersec %s (Terse language %s)\n", kCompilerVersion, kLanguageVersion);
            return 0;
        }
        else if (a == "-o") {
            if (i + 1 >= args.size()) { std::fputs("tersec: error: missing filename after '-o'\n", stderr); return 2; }
            outpath = args[++i];
        }
        else if (starts("-D")) {
            if (!next_arg(v)) return 2;
            std::size_t eq = v.find('=');
            if (eq != std::string::npos) pp.define(v.substr(0, eq), v.substr(eq + 1));
            else pp.define(v, "1");
        }
        else if (starts("-U")) { if (!next_arg(v)) return 2; pp.undef(v); }
        else if (starts("-I")) { if (!next_arg(v)) return 2; pp.add_include_dir(v); }
        else if (a == "-O0") opt = 0;
        else if (a == "-O1" || a == "-O") opt = 1;
        else if (a == "-O2" || a == "-O3") opt = 2;
        else if (a == "-E") only_pp = true;
        else if (a == "-fsyntax-only") syntax_only = true;
        else if (a == "--primer") primer = true;
        else if (a == "--stats") stats = true;
        else if (a == "--emit=terse") kind = EmitKind::Terse;
        else if (a == "--emit=json") kind = EmitKind::Json;
        else if (a == "--emit=english") kind = EmitKind::English;
        else if (a == "--emit=xml") kind = EmitKind::Xml;
        else if (a == "--emit=markdown" || a == "--emit=md") kind = EmitKind::Markdown;
        else if (a == "--emit=yaml") kind = EmitKind::Yaml;
        else if (a == "--format") format = true;
        else if (a == "-i") in_place = true;
        else if (a == "--check") check = true;
        else if (a == "--fix") fix = true;
        else if (a == "--dry-run") dry_run = true;
        else if (starts("--budget=")) {
            std::string_view n = std::string_view(a).substr(9);
            if (n.empty() || n.find_first_not_of("0123456789") != std::string_view::npos || n.size() > 9) {
                std::fprintf(stderr, "tersec: error: --budget needs a whole number of tokens, like --budget=200\n");
                return 2;
            }
            cli_budget = to_int(n);
        }
        else if (a == "--diagnostics-format=json") diag.set_format(Diagnostics::Format::Json);
        else if (a == "--diagnostics-format=text") diag.set_format(Diagnostics::Format::Text);
        else if (starts("--diagnostics-format=")) {
            std::fprintf(stderr, "tersec: error: unknown --diagnostics-format '%s' (use text or json)\n", a.c_str() + 21);
            return 2;
        }
        else if (starts("--emit=")) {
            std::fprintf(stderr, "tersec: error: unknown --emit target '%s' (use terse, json, english, xml, markdown or yaml)\n",
                         a.c_str() + 7);
            return 2;
        }
        else if (a == "--color=always" || a == "-fcolor-diagnostics") color = true;
        else if (a == "--color=never" || a == "-fno-color-diagnostics") color = false;
        else if (a == "--color=auto") color = isatty(2);
        else if (diag.option(a)) {}
        else if (a == "-") inputs.push_back(a);
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "tersec: error: unknown option '%s' (see tersec --help)\n", a.c_str());
            return 2;
        }
        else inputs.push_back(a);
    }
    diag.set_color(color && !std::getenv("NO_COLOR"));

    if (inputs.empty()) {
        if (args.size() <= 1) { usage(stderr); return 2; }
        std::fputs("tersec: fatal error: no input files\n", stderr);
        return 2;
    }
    if (format) return run_format(inputs, in_place, check);
    if ((in_place || check) ) {
        std::fputs("tersec: error: -i and --check only work with --format (terse fmt)\n", stderr);
        return 2;
    }
    if (dry_run && !fix) {
        std::fputs("tersec: error: --dry-run only works with --fix\n", stderr);
        return 2;
    }
    if (fix && std::find(inputs.begin(), inputs.end(), "-") != inputs.end()) {
        std::fputs("tersec: error: --fix needs a file name, not stdin\n", stderr);
        return 2;
    }
    if (outpath && inputs.size() > 1 && !syntax_only) {
        std::fputs("tersec: fatal error: cannot use -o with multiple input files\n", stderr);
        return 2;
    }

    std::string result;
    bool any_error = false;

    for (const std::string &input : inputs) {
        const int errs_before = diag.errors();
        const PLines pl = pp.run(input);
        const std::string_view shown = input == "-" ? "<stdin>" : std::string_view(input);

        if (only_pp) {
            result += join_lines(pl);
            continue;
        }

        Prompt p = parse(pl, shown, diag);
        sema.run(p, opt, errs_before);
        if (diag.errors() > errs_before) {
            any_error = true;
            if (fix) std::fprintf(stderr, "%s: not fixed, because it has errors\n", input.c_str());
            continue;
        }

        if (fix) {
            const Diagnostics::SrcFile *src = pp.main_source();
            std::optional<std::string> raw = read_file(input);
            if (!src || !raw) throw FatalError(cat(input, ": No such file or directory"));
            FixResult r = apply_fixes(p, *src, *raw);
            if (dry_run) {
                result += r.diff;
            } else if (r.changes) {
                if (!write_file(input, r.text)) throw FatalError(cat("cannot write '", input, "'"));
                std::fprintf(stderr, "%s: applied %d fix%s\n", input.c_str(), r.changes, r.changes == 1 ? "" : "es");
            }
            continue;
        }

        const std::string text = emit(p, kind, primer);
        std::optional<Preprocessor::Budget> budget = pp.budget();
        if (cli_budget) budget = Preprocessor::Budget{*cli_budget, Loc{shown, 0, 0, 0, nullptr}};
        const int out_tokens = approx_tokens(text);
        if (budget && out_tokens > budget->tokens) {
            report_budget(diag, p, out_tokens, budget->tokens, budget->loc);
            any_error = true;
            continue;
        }
        if (syntax_only) continue;
        if (inputs.size() > 1 && !result.empty()) result += "---\n";
        result += text;

        if (stats) {
            const int ts = approx_tokens(join_lines(pl));
            const int to = approx_tokens(text);
            const int te = approx_tokens(emit(p, EmitKind::English, false));
            const int tc = approx_tokens(emit(p, EmitKind::Terse, false));
            std::fprintf(stderr, "%.*s: approximate tokens (heuristic estimate, not a real tokenizer)\n",
                         int(shown.size()), shown.data());
            std::fprintf(stderr, "  source, preprocessed      %5d\n", ts);
            std::fprintf(stderr, "  output                    %5d\n", to);
            std::fprintf(stderr, "  plain-English equivalent  %5d\n", te);
            if (te > 0)
                std::fprintf(stderr, "  Terse vs English           %4d%% smaller\n",
                             int(100.0 * (te - tc) / te + 0.5));
            if (budget && budget->tokens > 0)
                std::fprintf(stderr, "  budget                    %5d (%d%% used)\n", budget->tokens,
                             int(100.0 * to / budget->tokens + 0.5));
        }
    }

    diag.finish();

    if (any_error || diag.errors()) return 1;
    if (syntax_only || no_output || (fix && !dry_run)) return 0;

    if (outpath) {
        if (!write_output(*outpath, result)) {
            std::fprintf(stderr, "tersec: fatal error: cannot write '%s'\n", outpath->c_str());
            return 2;
        }
    } else if (!result.empty()) {
        std::fwrite(result.data(), 1, result.size(), stdout);
    }
    return 0;
}

} // namespace
} // namespace terse

int main(int argc, char **argv) {
    terse::Diagnostics diag;
    try {
        return terse::run(std::vector<std::string>(argv, argv + argc), diag);
    } catch (const terse::FatalError &e) {
        diag.print_fatal(e.what());
    } catch (const std::bad_alloc &) {
        diag.print_fatal("out of memory");
    }
    return 2;
}
