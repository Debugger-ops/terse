# Terse 0.4 — Language Reference

Terse source lives in `.terse` files. `tersec` (pronounced "terse-see") compiles it the way `cc` compiles C:

```
prompt.terse ──► preprocessor ──► parser ──► semantic checks + lint ──► optimizer ──► emitter ──► prompt
               (#include,        (statements)  (errors, warnings)      (-O0/1/2)      (terse, json, english,
                #define, #use,                                                         xml, markdown, yaml)
                #for)
```

A file that has errors produces no output and `tersec` exits with status 1, just like a C compiler.

---

## 1. Compiling

```sh
make                          # builds ./tersec (C++17, no dependencies)
make test                     # runs the golden-file test suite
sudo make install             # optional: installs to /usr/local/bin

terse build prompt.terse       # print the compiled prompt
terse check prompt.terse       # errors and warnings only
terse stats prompt.terse       # approximate token counts only

tersec prompt.terse             # same as `terse build`, C-compiler style
tersec prompt.terse -o out.txt  # write it to a file
tersec prompt.terse | pbcopy    # macOS: copy it to the clipboard

terse fmt -i prompt.terse       # tidy the source in place
terse fix prompt.terse          # apply the optimizer's rewrites to the source
```

`terse` and `tersec` are the same program. `terse <command>` is the friendly form; `tersec` takes the flags below directly, like `cc`. Every flag also works after `terse build`.

| Command | Same as |
|---|---|
| `terse build FILE` | `tersec FILE` |
| `terse check FILE` | `tersec -fsyntax-only FILE` |
| `terse stats FILE` | `tersec --stats FILE`, without printing the prompt |
| `terse fmt FILE` | `tersec --format FILE` |
| `terse fix FILE` | `tersec --fix FILE` |
| `terse help` / `terse version` | `tersec --help` / `tersec --version` |

| Option | Effect |
|---|---|
| `-o FILE` | Write output to FILE (default stdout) |
| `--emit=terse` | Compact Terse prompt (default) |
| `--emit=json` | Structured JSON, for scripts and APIs |
| `--emit=english` | Plain-English version, for models or teammates that don't know Terse |
| `--emit=xml` | XML-style tags (§8) |
| `--emit=markdown` (or `md`) | Bold labels and lists (§8) |
| `--emit=yaml` | Same structure as JSON, as YAML (§8) |
| `--primer` | Put the Terse primer in front of the prompt |
| `-E` | Stop after the preprocessor and print its output |
| `-fsyntax-only` | Check only, write nothing |
| `--stats` | Print approximate token counts to stderr |
| `--budget=N` | Error if the output is over ~N tokens (§9); overrides `#pragma budget` |
| `--format` | Print the file in canonical layout (§10); add `-i` to rewrite it, `--check` to only test it |
| `--fix` | Write the optimizer's rewrites back into the file (§10); add `--dry-run` for a diff |
| `-O0` / `-O1` / `-O2` | Optimization level (default `-O1`, see §6) |
| `-D NAME[=value]` | Define NAME (value defaults to `1`) |
| `-U NAME` | Undefine NAME |
| `-I DIR` | Add DIR to the `#include` search path |
| `-Wall` / `-Wextra` | Default warnings / also the extra ones |
| `-W<name>` / `-Wno-<name>` | Turn one warning on or off |
| `-Werror` | Treat warnings as errors |
| `-w` | Silence all warnings |
| `--color=auto\|always\|never` | Colored diagnostics (also respects `NO_COLOR`) |
| `--diagnostics-format=json` | Diagnostics as one JSON array on stderr (§7) |
| `-` as a file name | Read from stdin |

Exit status: `0` compiled, `1` compile errors, `2` usage or fatal error.

---

## 2. Lexical rules

| Element | Rule |
|---|---|
| Lines | One statement per line. Blank lines are ignored. Leading indentation is ignored. |
| Line comment | `//` to end of line, only at the start of a line or after whitespace (so `https://x` is safe) |
| Block comment | `/* ... */`, may span lines |
| Code span | `` `text` `` — never rewritten, never substituted |
| Code fence | ```` ```lang ```` … ```` ``` ```` — multi-line value, kept byte-for-byte (comments too) |
| Quoted text | `"text"` — never rewritten by the optimizer |
| Substitution | `{NAME}` is replaced by a `#define` or template parameter; `\{` is a literal brace |
| Encoding | UTF-8; a leading BOM is skipped; `\r\n` line endings are accepted |

---

## 3. Grammar

```ebnf
file        = { line } ;
line        = directive | statement | comment | blank ;

statement   = field | include | exclude | rule | step ;
field       = key ":" value ;
include     = "+" SP value ;
exclude     = "-" SP value ;
rule        = "!" SP value ;
step        = DIGITS "." SP value ;

key         = "task" | "ctx" | "in" | "err" | "for" | "tone" | "ask"
            | "why" | "out" | "let" | "if" | "ref" | custom-key ;
value       = text [ fence ] ;
fence       = "```" [ lang ] NEWLINE { any-line } "```" ;

directive   = "#" name [ args ] ;
```

---

## 4. Statements

| Statement | Meaning | Repeat? | Checked by the compiler |
|---|---|---|---|
| `task:` | The action. Start with a verb. | **error** | Required unless the prompt has numbered steps |
| `ctx:` | Background | yes | — |
| `in:` | The input or where it is | yes | — |
| `err:` | Exact error text | yes | Never rewritten; warns if not quoted |
| `for:` | Audience | warning | — |
| `tone:` | Voice | warning | — |
| `ask:` | Request inside a drafted message | yes | — |
| `why:` | Reason | yes | — |
| `let:` | `name = value`, a name the model reuses | yes | Must be `name = value`; redefinition is an error; unused name warns |
| `if:` | `condition -> action` | yes | Must contain `->` |
| `ref:` | Points to an earlier part | yes | `step N` must exist |
| `out:` | Answer format and length | **error** | Missing `out:` warns |
| `+ x` | Must include | yes | — |
| `- x` | Must exclude | yes | — |
| `! x` | Hard rule | yes | — |
| `1. x` | Ordered step | — | Must be numbered 1, 2, 3…; `step N` references must point back to a step that exists |

Operators inside values (`->`, `|`, `?`, `=`, `~`) are unchanged from Terse 0.1 (see README §3.2).

### Custom keys

Unknown keys are an error (`tsk:` → *did you mean 'task'?*). To add your own, declare it first:

```
#pragma key lang
lang: typescript
```

---

## 5. Preprocessor

Works like C's. Directives start with `#` at the beginning of a line.

| Directive | Effect |
|---|---|
| `#include "file.terse"` | Insert a file. Searched next to the current file, then in `-I` dirs |
| `#include <file.terse>` | Search `-I` dirs only |
| `#pragma once` | Include this file at most once per compile |
| `#pragma key NAME` | Allow `NAME:` as a field key |
| `#define NAME value` | Compile-time constant, written as `{NAME}` |
| `#undef NAME` | Remove it |
| `#ifdef NAME` / `#ifndef NAME` | Keep lines if NAME is (not) defined |
| `#if NAME` | True if NAME is defined and not empty, `0` or `false` |
| `#if NAME == value` / `!=` | Compare text (`"quotes"` optional) |
| `#if !NAME` | Negation |
| `#elif …` / `#else` / `#endif` | As in C |
| `#template name(a, b)` … `#end` | Define a parameterised block (a "function") |
| `#template name(a, b="default")` | Parameters can have defaults; only trailing ones (like C++) |
| `#use name(x, y)` | Expand it. Arguments are split on commas; wrap an argument in `"…"` to keep commas in it. Missing trailing arguments take their defaults |
| `#for x in a, b, "c, d"` … `#end` | Repeat the block once per item with `{x}` bound. The list is substituted first, so `#for x in {LIST}` works. Loops nest and see the enclosing template's parameters |
| `#pragma budget N` | Error if the compiled prompt is over ~N tokens (§9) |
| `#error msg` / `#warning msg` | Stop the compile / print a warning |

`let:` vs `#define`: `#define` is expanded by the compiler and the model never sees the name. `let:` is sent to the model so it can refer to the name itself, which saves tokens when a long value is reused many times (R15).

### Example: a template library

```
// lib/common.terse
#pragma once
#define STACK React, Node, Postgres

#template bugfix(where, error)
task: fix bug
ctx: {STACK} app, {where}
err: {error}
out: changed lines only, 2 line why
#end
```

```
// debug.terse
#include "lib/common.terse"
#use bugfix("login form, submit click -> nothing", "'Cannot read properties of undefined (reading 'email')'")
- praise
```

```
$ tersec debug.terse
task: fix bug
ctx: React, Node, Postgres app, login form, submit click -> nothing
err: 'Cannot read properties of undefined (reading 'email')'
out: changed lines only, 2 line why
- praise
```

### Example: build variants with -D

```
#ifndef ROLE
#error ROLE is not set; compile with -D ROLE=frontend or -D ROLE=design
#endif
task: rewrite resume bullets
#if ROLE == frontend
+ React, performance, accessibility numbers
#elif ROLE == design
+ user research, design system, outcomes
#endif
out: 1 line each
```

`tersec -D ROLE=design resume.terse`

### Example: defaults and loops

```
#define ROLES frontend, "full-stack, Node", design

#template review(what, depth="top 3 issues", tone=direct)
task: review {what}
tone: {tone}
out: {depth}
#end

#use review("PR 42, auth flow")
#for r in {ROLES}
+ what a {r} reviewer would flag
#end
```

```
$ tersec review.terse
task: review PR 42, auth flow
tone: direct
out: top 3 issues
+ what a frontend reviewer would flag
+ what a full-stack, Node reviewer would flag
+ what a design reviewer would flag
```

Diagnostics inside a loop point at both the line and the iteration: `note: in '#for' iteration 'r = design' here`.

---

## 6. Optimizer

The optimizer never touches `err:` values, `"quoted text"`, `` `code` `` or fenced blocks.

| Level | Rewrites |
|---|---|
| `-O0` | None. Output is the preprocessed source, normalised. |
| `-O1` (default) | Removes filler (`please`, `could you`, `I want you to`, …); `→ ≤ ≥ ≠ — “ ” …` to ASCII; drops emoji; `k8s i18n a11y l10n o11y` to full words; `w/ w/o e.g.` to `with without like`; number words to digits (`five` → `5`, `one hundred` → `100`); removes duplicate lines. |
| `-O2` | Everything in `-O1`, plus drops articles (`a`, `an`, `the`) and reorders lines canonically: `task` → `let` → context keys → `+ - !` → steps → `out`. Can change meaning; read the output. |

---

## 7. Diagnostics

Messages follow gcc/clang format, with the source line and a caret:

```
review.terse:1:1: error: unknown key 'tsk'; did you mean 'task'?
    1 | tsk: summarize article
      | ^~~~
1 error generated.

fix.terse:1:11: warning: 'k8s' costs more tokens than 'kubernetes' (R8) [-Wnumeronym]
    1 | task: fix k8s config
      |           ^~~
1 warning generated.
```

### JSON diagnostics

`--diagnostics-format=json` prints one JSON array on stderr instead of text, for editors and CI. Notes are nested under the diagnostic they belong to; lint warnings carry a `fixit` with the replacement text for the underlined range. `line`, `column` and `length` are `null` when there is no exact position. Text written by `--stats` still goes to stderr, so don't combine the two if you parse stderr.

```json
[
  {"kind": "warning", "file": "fix.terse", "line": 1, "column": 11, "length": 3,
   "message": "'k8s' costs more tokens than 'kubernetes' (R8)", "option": "-Wnumeronym",
   "fixit": {"replacement": "kubernetes"},
   "expansions": [], "notes": []}
]
```

Each entry in `expansions` is `{"file", "line", "column", "type": "template" | "for", "name"}`, innermost first.

### Errors

| Error | Cause |
|---|---|
| `unknown key 'x'; did you mean 'y'?` | Key not in §4 and not declared with `#pragma key` |
| `expected 'key:', '+', '-', '!' or a step number…` | Line isn't a statement |
| `expected a value after 'task:'` | Empty value |
| `redefinition of 'task:'` / `'out:'` | Given twice (with a note pointing to the first) |
| `expected 'name = value' in 'let:'` / `redefinition of 'name'` | Malformed or repeated `let:` |
| `expected '->' in 'if:'` | Condition without an action |
| `step 3 out of sequence; expected step 2` | Steps must count 1, 2, 3… |
| `reference to step 5, but the prompt has only 3 steps` | `step N` that doesn't exist |
| `step 2 refers to step 3, which has not run yet` | Forward reference inside a step |
| `prompt has no 'task:' and no numbered steps` | Nothing to do (like a C program with no `main`) |
| `use of undeclared name 'X'` | `{X}` without a `#define` or parameter |
| `use of undeclared template` / `too few arguments to template` | Bad `#use` |
| `parameter 'b' … needs a default` | A parameter without a default after one with a default |
| `expected 'in' after '#for x'`, `unterminated '#for'` | Bad loop |
| `prompt is ~N tokens, over the budget of M` | Over `#pragma budget` / `--budget`; notes point at the 3 costliest lines |
| `'file' file not found`, `invalid preprocessing directive`, `unterminated …` | Preprocessor problems |

### Warnings

| Flag | Default | Rule | Fixed at |
|---|---|---|---|
| `-Wfiller` | on | R1 politeness and filler | `-O1` |
| `-Wunicode` | on | Non-ASCII symbols and emoji | `-O1` |
| `-Wnumeronym` | on | R8 `k8s`, `i18n`, `a11y`… | `-O1` |
| `-Wabbrev` | on | §6 `w/`, `w/o`, `e.g.` | `-O1` |
| `-Wnumber-words` | on | R10 `five` → `5` | `-O1` |
| `-Wduplicate` | on | R13 repeated line | `-O1` |
| `-Wmissing-out` | on | R17 no `out:` line | — |
| `-Wunused-let` | on | R15 `let:` name never used | — |
| `-Werr-quote` | on | R11 `err:` text not quoted | — |
| `-Wrepeat-key` | on | `for:` / `tone:` given twice | — |
| `-Wredefined` | on | `#define` changed value | — |
| `-Wuser` | on | `#warning` | — |
| `-Wunknown-pragma` | on | Unrecognised `#pragma` | — |
| `-Wfield-order` | `-Wextra` | `task:` not first / `out:` not last | `-O2` |
| `-Warticles` | `-Wextra` | R2 `a`, `an`, `the` | `-O2` |
| `-Wout-length` | `-Wextra` | R18 `out:` without a number | — |

---

## 8. Output targets

Source:

```
task: rewrite resume bullets
ctx: fresher, frontend roles
+ action verb first
! no invented facts
out: 1 line each
```

`--emit=json`

```json
{
  "task": "rewrite resume bullets",
  "ctx": "fresher, frontend roles",
  "out": "1 line each",
  "include": [
    "action verb first"
  ],
  "rules": [
    "no invented facts"
  ]
}
```

JSON layout: each field key maps to a string (repeated keys are joined with `\n`), `let:` becomes an object, and `+ - ! 1.` become the `include`, `exclude`, `rules` and `steps` arrays.

`--emit=yaml` has exactly the same structure as JSON (it parses to the same data). Values that YAML would misread (`yes`, `123`, `- x`, `a: b`…) are double-quoted; fenced values become `|-` blocks.

`--emit=xml`

```xml
<task>rewrite resume bullets</task>
<context>fresher, frontend roles</context>
<include>
  <item>action verb first</item>
</include>
<rules>
  <rule>no invented facts</rule>
</rules>
<output_format>1 line each</output_format>
```

XML-style tags are the structure Anthropic recommends for Claude prompts. Tags: `task`, `definitions`/`definition name=`, `context`, `input`, `error`, `audience`, `tone`, `ask`, `reason`, `reference`, `if condition=`, `include`, `exclude`, `rules`, `steps`/`step n=`, `output_format`; a custom key is its own tag. Text is kept verbatim, not entity-escaped, so code stays readable — the output is for a model, not an XML parser.

`--emit=markdown`

```
**Task:** rewrite resume bullets

**Context:** fresher, frontend roles

**Include:**

- action verb first

**Rules (never break):**

- no invented facts

**Output format:** 1 line each
```

`xml`, `markdown` and `english` all use the canonical order (task → definitions → context → include/exclude/rules → steps → output), whatever order the source uses.

`--emit=english`

```
Rewrite resume bullets.
Context: fresher, frontend roles.
Make sure to include: action verb first.
Hard rule, never break it: no invented facts.
Format the answer as: 1 line each.
```

---

## 9. Token statistics

`--stats` prints a rough estimate. **It is a heuristic, not a real tokenizer.** Use it to compare versions of the same prompt; check exact numbers with the model's own token-counting tool.

```
$ tersec -O2 --stats examples/messy.terse
examples/messy.terse: approximate tokens (heuristic estimate, not a real tokenizer)
  source, preprocessed         81
  output                       51
  plain-English equivalent     69
  Terse vs English             26% smaller
```

### Budgets

`#pragma budget N` (or `--budget=N`, which wins) makes a prompt over ~N tokens a compile error. The estimate is the same heuristic, applied to the exact output (including `--primer` and the chosen `--emit` target). Three notes point at the most expensive lines so you know what to cut:

```
review.terse:1:1: error: prompt is ~42 tokens, over the budget of 12 (by ~30)
    1 | #pragma budget 12
      | ^~~~~~~
review.terse:3:1: note: this line costs ~14 tokens
    3 | ctx: payments service, Go, high traffic, strict latency limits
      | ^~~~
```

With a budget set, `--stats` adds a line: `budget  150 (82% used)`.

---

## 10. Formatting and --fix

`terse fmt` is to Terse what `clang-format` is to C. It changes layout only: the compiled prompt is identical before and after.

| Before | After |
|---|---|
| `task:summarize   ` | `task: summarize` |
| `+    real examples` | `+ real examples` |
| `#  define   X  1` | `#define X  1` |
| 3 blank lines | 1 blank line |
| lines inside `#if` / `#template` / `#for` | indented 2 spaces per level |

Values, comments, `/* block comments */` and code fences are never changed. `terse fmt FILE` prints the result, `-i` rewrites the file, `--check` exits 1 if a file isn't formatted (for CI). Running it twice changes nothing.

`terse fix` runs the compiler and writes what the optimizer changed back into your source, so the source itself gets shorter. It uses the `-O` level you pass (`-O1` by default; `-O2` also drops articles, but never reorders lines in the source).

```
$ terse fix --dry-run -O2 prompt.terse
--- prompt.terse
+++ prompt.terse (fixed)
@@ -3 +3 @@
-task: please summarize the k8s incident
+task: summarize kubernetes incident
@@ -6,1 +5,0 @@
-+ timeline w/ five key events
```

It only edits single-line values written directly in that file. Lines that come from `#include`, `#use`, `#for` or `{NAME}` substitution, and fenced values, are left alone (the warning still tells you about them). A file with errors is never fixed.

---

## 11. Source layout

| File | Stage |
|---|---|
| `src/tersec.hpp` | Shared types: `Loc`, `Diagnostics`, `Preprocessor`, `Node`, `Prompt`, `Sema` |
| `src/main.cpp` | Command-line driver |
| `src/pp.cpp` | Preprocessor: comments, `#include`, `#define`, conditionals, templates |
| `src/parse.cpp` | Lines → statements |
| `src/sema.cpp` | Semantic checks, lint rules, optimizer |
| `src/emit.cpp` | `terse`, `json`, `english` output; token estimate; embedded primer |
| `src/diag.cpp` | Diagnostics with source snippets and carets |
| `src/fmt.cpp` | `terse fmt` |
| `src/fix.cpp` | `terse fix` |
| `src/util.cpp` | Strings, files, small algorithms |
| `tests/run.sh` | Golden-file tests (`BLESS=1 make test` to regenerate expectations) |
