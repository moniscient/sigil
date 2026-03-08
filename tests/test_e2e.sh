#!/bin/bash
# End-to-end tests: .skw -> C -> compile -> run -> verify output
set -e

SIGIL="./build/sigil"
CC="cc"
CFLAGS="-std=c11 -I src"
RUNTIME="src/sigil_runtime.c"
TMPDIR=$(mktemp -d)
PASS=0
FAIL=0

run_test() {
    local name="$1"
    local input="$2"
    local expected="$3"

    local skw_file="$TMPDIR/${name}.skw"
    local c_file="$TMPDIR/${name}.c"
    local bin_file="$TMPDIR/${name}"

    echo "$input" > "$skw_file"

    if ! $SIGIL "$skw_file" -c -o "$c_file" 2>/dev/null; then
        echo "FAIL: $name (sigil compilation failed)"
        FAIL=$((FAIL + 1))
        return
    fi

    if ! $CC $CFLAGS -o "$bin_file" "$c_file" "$RUNTIME" 2>/dev/null; then
        echo "FAIL: $name (C compilation failed)"
        cat "$c_file"
        FAIL=$((FAIL + 1))
        return
    fi

    local actual
    actual=$("$bin_file" 2>&1)

    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        echo "  expected: '$expected'"
        echo "  actual:   '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

run_sigil_test() {
    local name="$1"
    local input="$2"
    local expected="$3"

    local sigil_file="$TMPDIR/${name}.sigil"
    local c_file="$TMPDIR/${name}.c"
    local bin_file="$TMPDIR/${name}"

    echo "$input" > "$sigil_file"

    if ! $SIGIL "$sigil_file" -c -o "$c_file" 2>/dev/null; then
        echo "FAIL: $name (sigil compilation failed)"
        FAIL=$((FAIL + 1))
        return
    fi

    if ! $CC $CFLAGS -o "$bin_file" "$c_file" "$RUNTIME" 2>/dev/null; then
        echo "FAIL: $name (C compilation failed)"
        cat "$c_file"
        FAIL=$((FAIL + 1))
        return
    fi

    local actual
    actual=$("$bin_file" 2>&1)

    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name"
        echo "  expected: '$expected'"
        echo "  actual:   '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== End-to-End Tests ==="
echo

echo "--- Literals and bindings ---"

run_test "int_lit" \
    "print 42" \
    "42"

run_test "negative_int" \
    "print begin negate 7 end" \
    "-7"

run_test "bool_true" \
    "let x true
print x" \
    "true"

run_test "bool_false" \
    "let x false
print x" \
    "false"

echo "--- Arithmetic ---"

run_test "add" \
    "print begin add 3 4 end" \
    "7"

run_test "subtract" \
    "print begin subtract 10 3 end" \
    "7"

run_test "multiply" \
    "print begin multiply 6 7 end" \
    "42"

run_test "divide" \
    "print begin divide 100 4 end" \
    "25"

run_test "modulo" \
    "print begin modulo 17 5 end" \
    "2"

run_test "nested_arithmetic" \
    "print begin add begin multiply 3 4 end begin subtract 10 2 end end" \
    "20"

run_test "deeply_nested" \
    "print begin add begin add 1 2 end begin add 3 4 end end" \
    "10"

echo "--- Let / Var / Assign ---"

run_test "let_binding" \
    "let x 42
print x" \
    "42"

run_test "var_assign" \
    "var x 0
assign x 99
print x" \
    "99"

run_test "var_accumulate" \
    "var total 0
assign total begin add total 10 end
assign total begin add total 20 end
assign total begin add total 30 end
print total" \
    "60"

echo "--- Control flow: if ---"

run_test "if_true" \
    "if true begin
  print 1
end" \
    "1"

run_test "if_false_no_output" \
    "if false begin
  print 1
end
print 0" \
    "0"

run_test "if_else" \
    "if false begin
  print 1
end else begin
  print 2
end" \
    "2"

run_test "if_elif_else" \
    "if false begin
  print 1
end elif true begin
  print 2
end else begin
  print 3
end" \
    "2"

run_test "if_elif_chain" \
    "let x 3
if equal x 1 begin
  print 10
end elif equal x 2 begin
  print 20
end elif equal x 3 begin
  print 30
end else begin
  print 40
end" \
    "30"

echo "--- Control flow: while ---"

run_test "while_sum" \
    "var sum 0
var i 1
while less i 6 begin
  assign sum begin add sum i end
  assign i begin add i 1 end
end
print sum" \
    "15"

run_test "while_countdown" \
    "var n 5
var result 1
while greater n 0 begin
  assign result begin multiply result n end
  assign n begin subtract n 1 end
end
print result" \
    "120"

echo "--- Control flow: match ---"

run_test "match_case" \
    "match 2 begin
  case 1 begin
    print 10
  end
  case 2 begin
    print 20
  end
  case 3 begin
    print 30
  end
end" \
    "20"

run_test "match_default" \
    "match 99 begin
  case 1 begin
    print 10
  end
  default begin
    print 0
  end
end" \
    "0"

echo "--- Functions ---"

run_test "fn_simple" \
    "fn double int a returns int
  begin return begin add a a end end
print begin double 21 end" \
    "42"

run_test "fn_two_params" \
    "fn sum3 int a int b int c returns int
  begin return begin add a begin add b c end end end
print begin sum3 10 20 30 end" \
    "60"

run_test "fn_calling_fn" \
    "fn double int a returns int
  begin return begin add a a end end
fn quadruple int a returns int
  begin return begin double begin double a end end end
print begin quadruple 5 end" \
    "20"

run_test "fn_with_if" \
    "fn abs int x returns int
  begin
    if less x 0 begin
      return begin negate x end
    end else begin
      return x
    end
  end
print begin abs begin negate 42 end end" \
    "42"

run_test "fn_recursive_factorial" \
    "fn factorial int n returns int
  begin
    if equal n 0 begin
      return 1
    end else begin
      return begin multiply n begin factorial begin subtract n 1 end end end
    end
  end
print begin factorial 6 end" \
    "720"

run_test "fn_fibonacci" \
    "fn fib int n returns int
  begin
    if less n 2 begin
      return n
    end else begin
      return begin add begin fib begin subtract n 1 end end begin fib begin subtract n 2 end end end
    end
  end
print begin fib 10 end" \
    "55"

echo "--- Var params ---"

run_test "var_param_inc" \
    "fn inc var int x returns void
  begin assign x begin add x 1 end end
var n 5
inc n
print n" \
    "6"

run_test "var_param_double" \
    "fn double_it var int x returns void
  begin assign x begin multiply x 2 end end
var val 7
double_it val
print val" \
    "14"

echo "--- Sigil algebra (full pipeline) ---"

run_test "sigil_add_skw" \
    "let x begin add 3 4 end
print x" \
    "7"

run_test "sigil_precedence_skw" \
    "let x begin add 2 begin multiply 3 4 end end
print x" \
    "14"

run_test "sigil_negate_skw" \
    "let x begin negate 5 end
let y begin add x 10 end
print y" \
    "5"

run_test "sigil_multi_ops_skw" \
    "let x begin subtract 10 3 end
let y begin multiply x 2 end
let z begin add y 1 end
print z" \
    "15"

run_sigil_test "sigil_add_sigil" \
    "algebra A
fn add int a + int b returns int
precedence +
use A
  let x 3 + 4" \
    ""

run_sigil_test "sigil_precedence_sigil" \
    "algebra A
fn add int a + int b returns int
fn multiply int a * int b returns int
precedence + *
use A
  let x 2 + 3 * 4" \
    ""

echo "--- Multi-statement ---"

run_test "multi_print" \
    "print 1
print 2
print 3" \
    "1
2
3"

echo "--- Type-aware print ---"

run_test "print_bool_true" \
    "print true" \
    "true"

run_test "print_bool_false" \
    "print false" \
    "false"

run_test "print_float" \
    "print 3.14" \
    "3.14"

echo "--- Complex programs ---"

run_test "euclid_gcd" \
    "fn gcd int a int b returns int
  begin
    if equal b 0 begin
      return a
    end else begin
      return begin gcd b begin modulo a b end end
    end
  end
print begin gcd 48 18 end" \
    "6"

run_test "is_even_odd" \
    "fn iseven int n returns bool
  begin return begin equal begin modulo n 2 end 0 end end
var count 0
var i 0
while less i 10 begin
  if iseven i begin
    assign count begin add count 1 end
  end
  assign i begin add i 1 end
end
print count" \
    "5"

run_test "power" \
    "fn power int base int exp returns int
  begin
    if equal exp 0 begin
      return 1
    end else begin
      return begin multiply base begin power base begin subtract exp 1 end end end
    end
  end
print begin power 2 10 end" \
    "1024"

run_test "nested_while" \
    "var total 0
var i 0
while less i 3 begin
  var j 0
  while less j 4 begin
    assign total begin add total 1 end
    assign j begin add j 1 end
  end
  assign i begin add i 1 end
end
print total" \
    "12"

run_test "sequential_computation" \
    "var x 1
assign x begin add x 1 end
assign x begin add x 1 end
print x" \
    "3"

echo "--- Map operations ---"

run_test "map_set_get" \
    "var m mapnew
set m 1 42
let v begin get m 1 end
print v" \
    "42"

run_test "map_set_multiple" \
    "var m mapnew
set m 1 10
set m 2 20
set m 3 30
print begin get m 2 end" \
    "20"

run_test "map_has_true" \
    "var m mapnew
set m 5 99
if has m 5 begin
  print 1
end else begin
  print 0
end" \
    "1"

run_test "map_has_false" \
    "var m mapnew
if has m 5 begin
  print 1
end else begin
  print 0
end" \
    "0"

run_test "map_remove_count" \
    "var m mapnew
set m 1 10
set m 2 20
set m 3 30
remove m 2
print begin mapcount m end" \
    "2"

run_test "map_float_values" \
    "var m mapnew
set m 1 3.14
set m 2 2.71
print begin get m 1 end" \
    "3.14"

run_test "map_bool_values" \
    "var m mapnew
set m 1 true
set m 2 false
if get m 1 begin
  print 1
end else begin
  print 0
end" \
    "1"

run_test "map_in_fn" \
    "fn lookup map int int m int key returns int
  begin return begin get m key end end
var m mapnew
set m 10 42
print begin lookup m 10 end" \
    "42"

run_test "map_overwrite" \
    "var m mapnew
set m 1 10
set m 1 99
print begin get m 1 end" \
    "99"

echo "--- User-defined types ---"

run_test "udt_construct_get" \
    "type Point int x int y
let p Point 3 4
print begin get p x end" \
    "3"

run_test "udt_get_second_field" \
    "type Point int x int y
let p Point 10 20
print begin get p y end" \
    "20"

run_test "udt_set_field" \
    "type Point int x int y
var p Point 1 2
set p x 99
print begin get p x end" \
    "99"

run_test "udt_pass_to_fn" \
    "type Point int x int y
fn getx Point p returns int
  begin return begin get p x end end
let p Point 7 8
print begin getx p end" \
    "7"

run_test "udt_nested" \
    "type Point int x int y
type Line Point a Point b
let p1 Point 0 0
let p2 Point 3 4
let seg Line p1 p2
print begin get begin get seg b end x end" \
    "3"

run_test "udt_multiple_prints" \
    "type Point int x int y
let p Point 5 10
print begin get p x end
print begin get p y end" \
    "5
10"

echo "--- For loop / Range ---"

run_test "for_range_sum" \
    "var sum 0
for i in range 1 6 begin
  assign sum begin add sum i end
end
print sum" \
    "15"

run_test "for_range_print" \
    "for i in range 0 3 begin
  print i
end" \
    "0
1
2"

echo "--- Monomorphization ---"

run_test "mono_identity_int" \
    "fn identity T x returns T
  begin return x end
print begin identity 42 end" \
    "42"

run_test "mono_identity_float" \
    "fn identity T x returns T
  begin return x end
print begin identity 3.14 end" \
    "3.14"

run_test "mono_identity_both" \
    "fn identity T x returns T
  begin return x end
print begin identity 42 end
print begin identity 3.14 end" \
    "42
3.14"

run_test "mono_transitive" \
    "fn identity T x returns T
  begin return x end
fn wrap T x returns T
  begin return begin identity x end end
print begin wrap 42 end
print begin wrap 3.14 end" \
    "42
3.14"

echo "--- Trait dispatch ---"

run_test "trait_dispatch_int" \
    "fn show int x returns void
  begin print x end
fn show float x returns void
  begin print x end
fn display T x returns void
  begin show x end
display 42" \
    "42"

run_test "trait_dispatch_float" \
    "fn show int x returns void
  begin print x end
fn show float x returns void
  begin print x end
fn display T x returns void
  begin show x end
display 3.14" \
    "3.14"

run_test "trait_bounded_dispatch" \
    "fn show int x returns void
  begin print x end
fn show float x returns void
  begin print x end
fn display Showable T x returns void
  begin show x end
display 42" \
    "42"

echo "--- Trait begin/end blocks ---"

run_sigil_test "trait_begin_end_algebra" \
    "algebra Math
trait Showable T begin
  fn show T x returns void
end
fn display Showable T x returns void
  begin show x end
implement Showable for int begin
  fn show int x returns void
    begin print x end
end
implement Showable for float begin
  fn show float x returns void
    begin print x end
end
use Math
  display 42" \
    "42"

run_sigil_test "trait_begin_end_float" \
    "algebra Math
trait Showable T begin
  fn show T x returns void
end
fn display Showable T x returns void
  begin show x end
implement Showable for int begin
  fn show int x returns void
    begin print x end
end
implement Showable for float begin
  fn show float x returns void
    begin print x end
end
use Math
  display 3.14" \
    "3.14"

echo "--- Trait constraint enforcement ---"

# Test that missing trait impl produces an error (should fail compilation)
cat > "$TMPDIR/trait_constraint_error.sigil" << 'SIGIL'
algebra Math
trait Showable T begin
  fn show T x returns void
end
fn display Showable T x returns void
  begin show x end
implement Showable for int begin
  fn show int x returns void
    begin print x end
end
use Math
  display 3.14
SIGIL

if $SIGIL "$TMPDIR/trait_constraint_error.sigil" -c -o "$TMPDIR/trait_constraint_error.c" 2>/dev/null; then
    echo "FAIL: trait_constraint_error (should have failed: float does not implement Showable)"
    FAIL=$((FAIL + 1))
else
    PASS=$((PASS + 1))
fi

echo "--- String literals ---"

run_test "string_simple" 'let s "hello world"
print s' "hello world"

run_test "string_escape" 'let s "line one\nline two"
print s' "$(printf 'line one\nline two')"

run_test "string_length" 'let s "hello"
let n length s
print n' "5"

run_test "string_get_char" 'let s "hello"
let c get s 0
print c' "h"

run_test "string_comment" '"this is a comment"
print 42' "42"

run_test "string_triple_quote" 'let s """hello
world"""
print s' "$(printf 'hello\nworld')"

echo "--- New builtins ---"

run_test "times" "let x times 3 4
print x" "12"

run_test "compare_true" "let x compare 5 5
print x" "true"

run_test "compare_false" "let x compare 5 3
print x" "false"

run_test "less_equal_true" "let x less_equal 3 5
print x" "true"

run_test "less_equal_eq" "let x less_equal 5 5
print x" "true"

run_test "less_equal_false" "let x less_equal 6 5
print x" "false"

run_test "greater_equal_true" "let x greater_equal 5 3
print x" "true"

run_test "greater_equal_eq" "let x greater_equal 5 5
print x" "true"

run_test "greater_equal_false" "let x greater_equal 3 5
print x" "false"

run_test "length_map" "var m mapnew
set m 1 10
set m 2 20
set m 3 30
let n length m
print n" "3"

echo
echo "=== Results: $PASS passed, $FAIL failed ==="

rm -rf "$TMPDIR"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
