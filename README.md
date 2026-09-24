# Terse — a compact prompt language for AI

**Version 0.4** · Trimmed-down English for commanding AI models with fewer tokens and less ambiguity.

Terse is now a compiled language: write `.terse` files and build them with `tersec` (pronounced "terse-see"), a C-style compiler with a preprocessor (`#include`, `#define`, `#if`, templates), error checking, warnings and an optimizer. See [LANGUAGE.md](LANGUAGE.md).

```sh
make && make test
./terse build examples/debug.terse     # same as: ./tersec examples/debug.terse
```

Terse is not a brand-new vocabulary. Invented words and symbols are *more* expensive for AI because tokenizers are trained on English (`xqvr` = 3 tokens, `database` = 1). Terse instead keeps words the model already knows, removes everything that carries no instruction, and puts what's left into a fixed, predictable structure.

---

## 1. Measured results

Token counts from a Claude tokenizer (the one bundled with Anthropic's older Python SDK). Current models use different tokenizers, so exact numbers will vary, but the ratios hold.

| Prompt type | English | Terse | Saved |
|---|---|---|---|
| Code: write a function | 35 | 26 | 26% |
| Debug: React error | 66 | 40 | 39% |
| Writing: email | 39 | 31 | 21% |
| Reading: summary | 34 | 23 | 32% |
| Career: resume | 58 | 33 | 43% |
| Data: multi-step | 55 | 36 | 35% |
| Design: review | 50 | 30 | 40% |
| **Total** | **337** | **219** | **35%** |

Input savings are about a third. Controlling the *answer* length with `out:` usually saves more, because output tokens cost 3–5× more than input.

---

## 2. The five laws

1. **Every word must be an instruction or a fact.** If deleting it doesn't change the result, delete it.
2. **Common words beat clever abbreviations.** Use a word only if it's 1 token. Abbreviate only when that saves a token (see §6).
3. **Structure replaces grammar.** A `key: value` line says what a sentence would say, without the connecting words.
4. **Always say what the output looks like.** `out:` is the highest-value line in any prompt.
5. **Clarity beats compression.** If the model misreads a Terse prompt, write that part in plain English. One redo costs more than every token you saved.

---

## 3. Structure

A Terse prompt is a list of lines. Each line starts with a **field key** or an **operator**. Order: `task` → `ctx`/`in` → constraints → `out`.

```
task: <what to do>
ctx: <background the model needs>
in: <the input / what's attached>
+ <must include>
- <must exclude>
! <hard rule, never break>
out: <format, length, style of the answer>
```

Only `task:` is required. Add other lines only when they carry information.

### 3.1 Field keys

| Key | Meaning | Example |
|---|---|---|
| `task:` | The action. Start with a verb. | `task: summarize article` |
| `ctx:` | Background or situation | `ctx: fresher, frontend roles` |
| `in:` | The input or its location | `in: uploaded csv` |
| `err:` | Exact error text (never shorten it) | `err: 'x is undefined'` |
| `for:` | Audience | `for: beginner` / `for: recruiter` |
| `tone:` | Voice | `tone: formal, warm` |
| `ask:` | The request inside a message you're drafting | `ask: 1 week extension` |
| `why:` | Reason or motivation | `why: sick last week` |
| `out:` | Answer format and length | `out: table, top 3` |
| `let:` | Define a name to reuse later | `let: stack = React, Node, Postgres` |
| `if:` | Condition | `if: no errors -> reply ok` |
| `ref:` | Point to an earlier part | `ref: step 2` |

Keys cost 1 token plus the colon. Full words like `task` and `table` cost the same as `do` and `tbl`, so Terse keeps the readable form.

### 3.2 Operators

| Operator | Meaning | Example |
|---|---|---|
| `+` | Include / add | `+ type hints, docstring` |
| `-` | Exclude / skip | `- examples, praise` |
| `!` | Hard rule, must not be broken | `! no invented facts` |
| `->` | Becomes / produces / then | `list[int] -> evens` |
| `\|` | Or / alternatives | `out: table \| bullets` |
| `?` | Question, or "you decide" | `lib: ?` |
| `1.` `2.` `3.` | Ordered steps, run in sequence | `1. clean data` |
| `=` | Defines or equals | `let: tone = casual` |
| `~` | Approximately | `out: ~200 words` |

Use plain ASCII: `->` (1 token), not `→`; `<=` (1), not `≤` (2).

---

## 4. Rules

### Grammar

- **R1.** Drop politeness and filler: *please, could you, kindly, I want you to, can you help me*.
- **R2.** Drop articles (*a, an, the*) and "to be" verbs (*is, are*) unless removing them changes the meaning.
- **R3.** Use imperatives: `task: fix bug`, not "I'd like you to fix a bug".
- **R4.** Put the noun first, then the qualifier: `bullets, 5`, `table, top 3`.
- **R5.** Commas separate items in a list. New lines separate ideas. They cost the same, so pick whichever is clearer.

### Words

- **R6.** Keep a word if it is 1 token (most common English words). Don't shorten `database`, `config`, `button`, `without`.
- **R7.** Abbreviate only multi-token words that have a standard short form: `ts` for *typescript*.
- **R8.** Never use numeronyms (`k8s`, `i18n`, `a11y`). They cost more than the full word.
- **R9.** Never invent words or codes. The model has to guess, and guessing breaks prompts.
- **R10.** Write numbers as digits: `5 bullets`, not "five bullet points".

### Things you never compress

- **R11.** Code, error messages, file names, URLs, names, quotes, and data. Paste them exactly.
- **R12.** Anything where losing a word could flip the meaning (legal, medical, money, safety). Write it in full English.

### Structure

- **R13.** One idea per line. Nothing that restates an earlier line.
- **R14.** Multi-step tasks use numbered steps. Refer back with `step N`, not by repeating the step.
- **R15.** Anything you mention more than twice goes in `let:` and gets reused by name.
- **R16.** Context you send every time goes in the system prompt or a skill, sent once, not in every message.

### Output control

- **R17.** Every prompt that expects more than a line back has an `out:`.
- **R18.** State length as a number: `max 100 words`, `3 bullets`, `1 line`.
- **R19.** Use a `-` to name the padding you don't want: `- intro, summary, praise, disclaimers`.
- **R20.** For code changes, ask for the diff only: `out: changed lines only`.

---

## 5. Output modes (`out:` values)

| Value | Answer you get |
|---|---|
| `out: terse` | Trimmed English, no intro or wrap-up |
| `out: code` | Code block only |
| `out: code, 1 line note` | Code and a one-line explanation |
| `out: changed lines only` | Only the lines that change |
| `out: table` | A table (add columns: `table \| a \| b \|`) |
| `out: 5 bullets` | Exactly 5 bullets |
| `out: y/n` | Yes or no only |
| `out: y/n + why` | Yes/no and one sentence |
| `out: json` | Valid JSON only |
| `out: max N words` | Hard length cap |
| `out: terse syntax` | The model answers in Terse too |

---

## 6. Word list (measured)

**Shortening doesn't help.** Both forms cost 1 token, so use the full word:

| Full word (keep) | Short form (no saving) |
|---|---|
| database | db |
| configuration | cfg |
| authentication | auth |
| repository | repo |
| application | app |
| component | comp |
| button | btn |
| response | resp |
| information | info |
| previous | prev |
| maximum / minimum | max / min |
| because | bc |
| table | tbl |
| implement | impl |

Common short forms like `db`, `app`, `max` are fine because they cost the same. Just don't expect them to save anything.

**Shortening helps.** Use the short form:

| Full | Tokens | Short | Tokens |
|---|---|---|---|
| typescript | 2 | ts | 1 |
| Please could you kindly | 4 | *(delete)* | 0 |
| five bullet points | 3 | 5 bullets | 2 |

**The short form costs more.** Use the full form:

| Short | Tokens | Full | Tokens |
|---|---|---|---|
| k8s | 3 | kubernetes | 2 |
| i18n | 3 | internationalization | 2 |
| e.g. | 4 | eg / like | 1 |
| w/ | 2 | with | 1 |
| w/o | 3 | without | 1 |
| ≤ | 2 | <= | 1 |
| $var | 2 | var | 1 |
| emoji | 2–4 | a word | 1 |

---

## 7. Examples

**Code**

> Can you please write a Python function that takes a list of numbers and returns only the even ones? Please add type hints and a docstring, and keep the explanation really short. — *35 tokens*

```
task: python fn, list[int] -> evens
+ type hints, docstring
out: code, 1 line note
```
*26 tokens*

**Debugging**

> I'm getting an error in my React app. When I click the submit button on the login form, nothing happens and the console shows 'Cannot read properties of undefined (reading 'email')'. Here is my code. Can you find the bug and explain how to fix it? Please only show the lines that need to change. — *66 tokens*

```
task: fix bug
ctx: React login form, submit click -> nothing
err: 'Cannot read properties of undefined (reading 'email')'
out: changed lines only, 2 line why
```
*40 tokens*

**Email**

> Could you help me write a short, polite email to my professor asking for a one-week extension on my project submission because I was sick last week? Keep it formal and under 100 words. — *39 tokens*

```
task: email to professor
ask: 1 week extension, project
why: sick last week
tone: formal, polite
out: max 100 words
```
*31 tokens*

**Summary**

> Please summarize the following article in five bullet points. Focus only on the main arguments and skip any examples or background information. Use simple language that a beginner can understand. — *34 tokens*

```
task: summarize article
out: 5 bullets
+ main arguments
- examples, background
for: beginner
```
*23 tokens*

**Resume**

> I am a fresher applying for frontend developer roles. Rewrite the following resume bullet points to be more impactful, start each one with a strong action verb, add numbers where possible, and keep each bullet to one line. Don't invent any achievements that aren't in the original. — *58 tokens*

```
task: rewrite resume bullets
ctx: fresher, frontend roles
+ action verb first, numbers if present
out: 1 line each
! no invented facts
```
*33 tokens*

**Multi-step data task**

> First, read the CSV file I uploaded and tell me how many rows and columns it has. Then find any missing values in each column. After that, create a bar chart showing the number of missing values per column. Finally, suggest how I should handle the missing data. — *55 tokens*

```
in: uploaded csv
1. count rows, cols
2. missing values per col
3. bar chart of step 2
4. suggest fix
out: terse
```
*36 tokens*

**Design review**

> I'm designing a mobile checkout screen for a food delivery app. Can you review my design and tell me the three biggest usability problems, ranked by severity, with a specific fix for each? I don't need general praise, just the issues. — *50 tokens*

```
task: review design
ctx: mobile checkout, food delivery app
out: table | issue | severity | fix |, top 3
- praise
```
*30 tokens*

---

## 8. Anti-patterns

| Don't | Why | Do |
|---|---|---|
| `tsk: smrz art 5b` | Invented shortenings break into several tokens and confuse the model | `task: summarize article` / `out: 5 bullets` |
| `🔥 fix ⚡ fast` | Emoji cost 2–4 tokens and mean nothing precise | `task: fix bug` |
| `err: undefined email thing` | Paraphrased errors lose the clue that matters | Paste the exact error |
| No `out:` line | The model picks its own length, usually long | Always set `out:` |
| Resending the same background in every message | You pay for it every time | Put it in the system prompt once, or use `let:` |
| Terse for a contract clause | One dropped word can change the meaning | Plain English (R12) |

---

## 9. Using Terse

- **Current Claude and GPT models** mostly understand Terse with no setup, because it's still English.
- **For consistent results**, paste `PRIMER.md` into the system prompt, project instructions, or custom instructions once. It costs about 150 tokens one time and makes the model treat the keys and operators exactly as defined here.
- **In long chats**, add `out: terse syntax` so the model's replies are compact too. Replies are where most tokens go.

---

## 10. Compiling Terse with tersec

| Command | Result |
|---|---|
| `terse build prompt.terse` | Checks the prompt and prints the compiled Terse (same as `tersec prompt.terse`) |
| `terse check prompt.terse` | Errors and warnings only |
| `terse stats prompt.terse` | Approximate token counts only |
| `tersec -O2 prompt.terse` | Also drops articles and puts lines in canonical order |
| `tersec --emit=json prompt.terse` | JSON for scripts and APIs |
| `tersec --emit=english prompt.terse` | Plain English, for teammates or models that don't know Terse |
| `tersec --primer prompt.terse` | Puts the primer in front, for a model seeing Terse for the first time |
| `tersec -D ROLE=design resume.terse` | Builds one variant of a prompt with `#if ROLE == design` |
| `tersec -Werror prompt.terse` | Fails on any rule violation (useful in CI) |
| `terse fmt -i prompt.terse` | Rewrites the file in the canonical layout (`--check` for CI) |
| `terse fix prompt.terse` | Writes the optimizer's rewrites back into your source (`--dry-run` shows a diff) |
| `tersec --budget=150 prompt.terse` | Fails if the prompt is over ~150 tokens and names the costliest lines (or `#pragma budget 150`) |
| `tersec --emit=xml prompt.terse` | XML-style tags (`<task>`, `<rules>`…), the layout Claude reads best |
| `tersec --emit=markdown` / `--emit=yaml` | Markdown for chat UIs and docs / YAML for config files |
| `tersec --diagnostics-format=json` | Errors and warnings as JSON, for editors |

The compiler enforces the rules in §4: unknown keys, a second `task:`, broken step numbering and `if:` without `->` are errors; filler, emoji, numeronyms, number words and a missing `out:` are warnings, and `-O1` (the default) fixes the fixable ones. Full reference: [LANGUAGE.md](LANGUAGE.md).

---

## 11. Limits

- Savings on input are about 20–45%. Most of the cost in real use is output length and repeated context. Terse covers both with `out:` and R15/R16, but prompt caching and starting new chats matter just as much.
- Token counts in this file come from one Claude tokenizer. Check your own numbers with the model's token-counting tool before relying on exact figures.
- Terse trades readability for other people. For prompts a team will maintain, plain English may be the better choice.

---

## Changelog

- **0.4** (Sep 2026): `terse fmt` (canonical layout, `-i`, `--check`) and `terse fix` (writes optimizer rewrites back to the source, `--dry-run` diff). Token budgets: `#pragma budget N` / `--budget=N`. New targets: `--emit=xml`, `--emit=markdown`, `--emit=yaml`. Templates get default arguments; new `#for x in a, b, c … #end` loops. `--diagnostics-format=json` with fix-its for editors. 11 new golden tests.

- **0.3** (Sep 2026): `tersec` rewritten in C++17 (same language, same output, byte-for-byte). RAII and standard containers replace manual `malloc`/`free`; each input file now truly starts from only the `-D`/`-U` names; `--stats` no longer hangs on stray control characters. The original C99 source is kept in `legacy/c/`.

- **0.2** (Sep 2026): Terse becomes a compiled language. `tersec` compiler in C99: preprocessor (`#include`, `#pragma once`, `#define`, `#if/#elif/#else`, `#template`/`#use`), `//` and `/* */` comments, code fences, gcc-style errors and warnings (`-Wall`, `-Wextra`, `-Werror`), optimizer (`-O0/-O1/-O2`), `terse`/`json`/`english` output, golden-file tests.

- **0.1** (Sep 2026): First spec. 12 field keys, 9 operators, 20 rules, measured word list, 7 examples.
