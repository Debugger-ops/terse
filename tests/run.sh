#!/bin/sh
# Golden-file tests. Each tests/cases/NAME.terse starts with "// FLAGS: ..." and has
#   NAME.out  expected stdout
#   NAME.err  expected stderr (colors off)
#   NAME.code expected exit status
# Run with BLESS=1 to (re)write the expected files from the current compiler.
cd "$(dirname "$0")/.." || exit 2
TERSEC=${TERSEC:-./tersec}
pass=0; fail=0
tmp=${TMPDIR:-/tmp}/tersec-test.$$
mkdir -p "$tmp"
for src in tests/cases/*.terse; do
    name=${src%.terse}
    flags=$(sed -n '1s|^// FLAGS:||p' "$src")
    # shellcheck disable=SC2086
    $TERSEC --color=never $flags "$src" >"$tmp/out" 2>"$tmp/err"
    echo $? >"$tmp/code"
    if [ -n "$BLESS" ]; then
        cp "$tmp/out" "$name.out"; cp "$tmp/err" "$name.err"; cp "$tmp/code" "$name.code"
        echo "blessed $name"; continue
    fi
    ok=1
    for k in out err code; do
        if ! cmp -s "$tmp/$k" "$name.$k"; then
            ok=0
            echo "FAIL $name ($k differs)"
            diff -u "$name.$k" "$tmp/$k" | head -20
        fi
    done
    if [ $ok = 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
done
rm -rf "$tmp"
[ -n "$BLESS" ] && exit 0
echo "$pass passed, $fail failed"
[ $fail = 0 ]
