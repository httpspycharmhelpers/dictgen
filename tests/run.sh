#!/usr/bin/env bash
#
# dictgen test suite
#   ./tests/run.sh          run all tests
#
set -u
cd "$(dirname "$0")/.."
BIN=${BIN:-./dictgen}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n     expected: %s\n     got:      %s\n' "$1" "$2" "$3"; }

# expect <name> <expected> <actual>
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi; }

# calc <expr> [digits]
calc() { if [ -n "${2:-}" ]; then $BIN calc -D "$2" "$1"; else $BIN calc "$1"; fi; }

echo "== build =="
make -s || exit 1

echo "== integer arithmetic =="
expect "add/mul precedence" "14"      "$(calc '2+3*4')"
expect "subtraction"        "-6"      "$(calc '3-9')"
expect "unary minus"        "-5"      "$(calc '-5')"
expect "parens"             "21"      "$(calc '(1+2)(3+4)')"
expect "exact division"     "25"      "$(calc '100/4')"
expect "inexact division"   "2.5"     "$(calc '10/4')"
expect "modulo"             "1"       "$(calc '7%3')"
expect "power"              "1267650600228229401496703205376" "$(calc '2^100')"
expect "right-assoc power"  "512"     "$(calc '2^3^2')"
expect "decimal literal"    "6.28"    "$(calc '3.14*2')"
expect "decimal + int"      "4"       "$(calc '1.5+2.5')"
expect "negative decimal"   "-7"      "$(calc '2*-3.5')"

echo "== factorials =="
expect "10!"                "3628800" "$(calc '10!')"
expect "20!"                "2432902008176640000" "$(calc '20!')"
expect "fact()"             "120"     "$(calc 'fact(5)')"
expect "0!"                 "1"       "$(calc '0!')"
expect "negative ! errors"  "1"       "$($BIN calc '(0-3)!' >/dev/null 2>&1; echo $?)"

echo "== real arithmetic =="
expect "pi 50 digits" \
  "3.1415926535897932384626433832795028841971693993751" "$(calc 'pi' 50)"
expect "2*pi*10 (30 digits)" \
  "62.8318530717958647692528676656" "$(calc '2*pi*10' 30)"
expect "sqrt(2) (20 digits)" \
  "1.4142135623730950488" "$(calc 'sqrt(2)' 20)"
expect "pi^2 (20 digits)" \
  "9.8696044010893586188" "$(calc 'pi^2' 20)"
expect "implicit mult 2pi" \
  "6.2831853071795864769" "$(calc '2pi' 20)"
expect "e constant" \
  "2.7182818284590452354" "$(calc 'e' 20)"

echo "== digit() =="
expect "digit(pi,1)"        "1" "$(calc 'digit(pi,1)')"
expect "digit(pi,2)"        "4" "$(calc 'digit(pi,2)')"
expect "digit(pi,5)"        "9" "$(calc 'digit(pi,5)')"
expect "digit(2*pi,32)"     "0" "$(calc 'digit(2*pi,32)' 50)"
expect "digit needs -D"     "1" "$($BIN calc 'digit(pi,999)' >/dev/null 2>&1; echo $?)"

echo "== big outputs (digit counts) =="
expect "2^1000000 digits"   "301030" "$(calc '2^1000000' | tr -d '\n' | wc -c | tr -d ' ')"
expect "100000! digits"     "456574" "$(calc '100000!' | tr -d '\n' | wc -c | tr -d ' ')"

echo "== pi subcommand =="
expect "pi 20 decimals" \
  "3.14159265358979323846" "$($BIN pi 20)"
expect "pi 10 decimals" \
  "3.1415926536" "$($BIN pi 10)"

echo "== precision aliases =="
expect "--digits N (calc)" \
  "62.8318530717958647692528676656" "$($BIN calc "2*pi*10" --digits 30)"
expect "--digits=N (calc)" \
  "62.8318530717958647692528676656" "$($BIN calc "2*pi*10" --digits=30)"
expect "--places N (calc)" \
  "62.8318530717958647692528676656" "$($BIN calc "2*pi*10" --places 30)"
expect "--digits still charset (gen)" \
  "0 0 0 1 0 2 0 3" "$($BIN -l 2 --digits -n 4 --format space -t | tr '\n' ' ' | sed 's/ $//')"

echo "== errors =="
expect "division by zero"   "1" "$($BIN calc '1/0' >/dev/null 2>&1; echo $?)"
expect "syntax error"       "1" "$($BIN calc '1+' >/dev/null 2>&1; echo $?)"
expect "unknown function"   "1" "$($BIN calc 'nope(1)' >/dev/null 2>&1; echo $?)"

echo "== generation: exact enumeration =="
expect "len1 ab"  "$(printf 'a\nb')"        "$($BIN -l 1 -c ab -t)"
expect "len2 ab"  "$(printf 'aa\nab\nba\nbb')" "$($BIN -l 2 -c ab -t)"
expect "len1-2 ab" "$(printf 'a\nb\naa\nab\nba\nbb')" "$($BIN -m 1 -M 2 -c ab -t)"
expect "len3 01 last row" "111" "$($BIN -l 3 -c 01 -t | tail -1)"
expect "count limit"  "4" "$($BIN -l 8 -c 0123 -n 4 -t | wc -l | tr -d ' ')"
expect "prefix/suffix" "$(printf 'Xa9\nXb9')" "$($BIN -l 1 -c ab -p X -s 9 -t)"
expect "format space" "$(printf 'a a\na b')" "$($BIN -l 2 -c ab --format space -t | head -2)"
expect "format escape" "$(printf '\\x61\\x61')" "$($BIN -l 2 -c ab --format escape -t | head -1)"
expect "ascii names" "NUL" "$($BIN -l 8 --binary --ascii-names -t | head -1)"
expect "-tff alias" "$(printf 'a\nb')" "$($BIN -l 1 -c ab -tff)"

echo "== output containers =="
printf 'foo\nbar\nbaz\n' > "$TMP/dict.txt"
expect "word mode" "$(printf 'foo\nbar\nbaz')" "$($BIN -a word -d "$TMP/dict.txt" -t)"
$BIN -l 2 -c abc -o "$TMP/g.tar" >/dev/null
expect "tar extract" "$($BIN -l 2 -c abc -t)" "$(tar xOf "$TMP/g.tar")"
expect "tar entry name" "g.txt" "$(tar tf "$TMP/g.tar")"
$BIN -l 2 -c abc -o "$TMP/g.tar.gz" >/dev/null
expect "tar.gz extract" "$($BIN -l 2 -c abc -t)" "$(tar xOzf "$TMP/g.tar.gz")"
$BIN calc '2^128' -o "$TMP/c.tar.gz" >/dev/null
expect "calc to tar.gz" "340282366920938463463374607431768211456" "$(tar xOzf "$TMP/c.tar.gz")"
$BIN -l 2 -c abc -o "$TMP/plain.txt" >/dev/null
expect "plain file" "$($BIN -l 2 -c abc -t)" "$(cat "$TMP/plain.txt")"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
