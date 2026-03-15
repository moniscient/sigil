# Resolved Bugs

Bugs moved here from `BUGS.md` after being fixed. BUG IDs are never reused.

---

### BUG-001: Trait constraint enforcement silently passed for unimplemented traits

- **Category**: Semantic
- **Severity**: High
- **Found**: 2026-03-08
- **Status**: Fixed (2026-03-08)

**Description**: Calling a trait-bounded generic function with a type that does not implement the required trait silently succeeded instead of producing a resolver error. For example, `display 3.14` where `display` requires `Showable T` and only `int` implements `Showable` — the compiler produced valid C output instead of rejecting the program.

**Root Cause**: The desugarer's `desugar_node` switch statement did not list `NODE_LAMBDA`, `NODE_COMPREHENSION`, `NODE_BREAK`, or `NODE_CONTINUE`. Since the switch had no `default:` case, hitting these node types was undefined behavior. This corrupted state in a way that caused the trait registry to not be properly populated by the time the resolver ran.

**Fix**: Added the missing node types to the desugarer's switch as no-op leaf cases.

**Location**: `src/desugarer.c`, `desugar_node` (~line 940)

---

### BUG-002: Comprehension filter produced sparse output maps

- **Category**: Codegen
- **Severity**: High
- **Found**: 2026-03-08
- **Status**: Fixed (2026-03-08)

**Description**: When a comprehension's `where` clause filtered out an element, the emitted C code incremented `_comp_idx` before `continue`, causing filtered-out items to consume sequential indices. This produced sparse maps with gaps.

**Example**: `collect from i in range 1 6 where greater i 3 apply i` would produce `{3: 4, 5: 5}` instead of `{0: 4, 1: 5}`.

**Root Cause**: The emitted code was:
```c
if (!(cond)) { _comp_idx++; continue; }
```
The `_comp_idx++` before `continue` consumed an index for filtered items.

**Fix**: Changed to `if (!(cond)) continue;` — the index only increments after a successful store.

**Location**: `src/c_emitter.c`, comprehension emission (~line 1034)

---

### BUG-003: Transitive monomorphization missed several AST node types

- **Category**: Codegen
- **Severity**: High
- **Found**: 2026-03-08
- **Status**: Fixed (2026-03-08)

**Description**: `collect_transitive_mono` — which walks generic function bodies to discover calls to other generic functions that need specialization — only recursed into `NODE_BLOCK`, `NODE_BEGIN_END`, `NODE_LET`, `NODE_VAR`, `NODE_ASSIGN`, `NODE_RETURN`, `NODE_IF`, and `NODE_WHILE`. Generic calls inside `for` loops, `match` statements, `case`/`default` arms, lambdas, or comprehensions were silently ignored, producing missing specializations and linker errors.

**Fix**: Added recursion into `NODE_FOR`, `NODE_MATCH`, `NODE_CASE`, `NODE_DEFAULT`, `NODE_LAMBDA`, and `NODE_COMPREHENSION`.

**Location**: `src/c_emitter.c`, `collect_transitive_mono` (~line 1675)

---

### BUG-004: Print on non-string maps and UDTs was undefined behavior

- **Category**: Codegen
- **Severity**: Critical
- **Found**: 2026-03-08
- **Status**: Fixed (2026-03-08)

**Description**: When `print` was called on a `map<int, int>` (or any non-string map) or a user-defined type (`TYPE_NAMED`), the emitter dispatched to `sigil_print_int()`. This passed a `SigilMap*` pointer to `printf("%lld")`, producing garbage output and undefined behavior.

**Root Cause**: The print type dispatch in `emit_expr` had:
```c
case TYPE_MAP:
    if (arg_type->val_type && arg_type->val_type->kind == TYPE_CHAR)
        print_fn = "sigil_print_string";
    else
        print_fn = "sigil_print_int";  // BUG: SigilMap* as int64_t
    break;
```
And `TYPE_NAMED` fell through to the `default: sigil_print_int` case.

**Fix**: Non-string maps and `TYPE_NAMED` now dispatch to `sigil_print_map()`, a new runtime function that formats as `{key: value, ...}`.

**Location**: `src/c_emitter.c`, print dispatch in `emit_expr` (~line 776); `src/sigil_runtime.c`, new `sigil_print_map()` function.

---

### BUG-005: `parse_condition_call` cannot accept `begin/end` expression groups as arguments

- **Category**: Parser
- **Severity**: Medium
- **Found**: 2026-03-08
- **Status**: Fixed (2026-03-08)

**Description**: `parse_condition_call` (used for `if`, `elif`, `while` conditions) checked `!at(p, TOK_BEGIN)` in its while-loop guard, causing `begin/end` expression groups inside conditions to be misinterpreted as the body delimiter.

**Root Cause**: The loop guard unconditionally stopped at any `TOK_BEGIN`, without checking whether it was a matched `begin/end` pair (grouped expression) or an unmatched `begin` (body delimiter).

**Fix**: Added `begin_is_grouped_expr()` helper that scans forward to check if a `begin` has a matching `end` on the same line. The condition loop now only stops at unmatched `begin` tokens. Same fix applied to the ident-call check in `parse_condition`.

**Location**: `src/parser.c`, `parse_condition_call` and `parse_condition`

---

### BUG-006: Imported fn bodies emitted as top-level statements in main()
- **Discovered**: 2026-03-08, during matmul_test.sigil compilation
- **Category**: Codegen
- **Severity**: High
- **Fix**: Removed NODE_IMPORT recursion from `collect_top_stmts` — imported fn declarations are emitted as separate C functions by `collect_fns`/`emit_fn_decl`, not as top-level statements.
- **Files**: src/c_emitter.c
- **Status**: Fixed (2026-03-08)

---

### BUG-007: Builtin `multiply` shadows user-defined multiply for non-int types
- **Discovered**: 2026-03-08, during matmul_test.sigil compilation
- **Category**: Codegen
- **Severity**: High
- **Fix**: Added type-aware builtin bypass — when any arg has TYPE_MAP or TYPE_NAMED, skip the builtin table and fall through to user-defined fn call emission.
- **Files**: src/c_emitter.c
- **Status**: Fixed (2026-03-08)

---

### BUG-008: Map op `get` with 3+ args emits nothing
- **Discovered**: 2026-03-08, during matmul_test.sigil compilation
- **Category**: Codegen
- **Severity**: High
- **Fix**: (1) Added else clause in map ops block to `goto map_op_fallthrough` when no handler matches. (2) Added 3-arg `get` handler for double-indexed map access (matrix m[i][j]) and 4-arg `set` handler.
- **Files**: src/c_emitter.c
- **Status**: Fixed (2026-03-08)

---

### BUG-009: `row`/`matrix` repeats fns emit calls to non-existent runtime functions
- **Discovered**: 2026-03-08, during matmul_test.sigil compilation
- **Category**: Runtime
- **Severity**: High
- **Fix**: (1) Added `sigil_row()` and `sigil_matrix()` to runtime. (2) Added `has_repeats`/`repeats_start_idx` to FnEntry. (3) Call-site emission now packs repeats args into count + compound literal.
- **Files**: src/sigil_runtime.c, src/sigil_runtime.h, src/types.h, src/types.c, src/c_emitter.c
- **Status**: Fixed (2026-03-08)

---

### BUG-010: Desugarer `use` block reparsing loses `if`/`while` conditions containing `begin/end` groups
- **Discovered**: 2026-03-09, during matmul_test.sigil verification loop
- **Category**: Semantic
- **Severity**: High
- **Description**: When an `if`, `while`, or `elif` condition inside a `use` block contained `begin/end` grouped expressions (e.g., `if not begin equal begin get C i j end 15 end begin ... end`), the desugarer's `reparse_use_body` naively scanned for the first `TOK_BEGIN` to find the body delimiter. This matched expression-level `begin` tokens instead, truncating the condition to zero tokens and producing a NULL condition node.
- **Root Cause**: `ep_parse_use_stmt`'s `if`/`while`/`elif` handlers used a linear scan (`while (tok[i].kind != TOK_BEGIN) i++`) to find where the condition ends and the body begins. This can't distinguish expression `begin/end` pairs from the body delimiter `begin`.
- **Fix**: Added `ep_parse_condition()` function that properly parses keyword-prefix calls (consuming `begin/end` args via `ep_parse_begin_end`) and stops at primitive keywords or structural keywords, naturally stopping before the body delimiter. Replaced all scan-for-begin logic in `if`, `elif`, `while`, and `for` handlers.
- **Files**: src/desugarer.c
- **Status**: Fixed (2026-03-09)

---

### BUG-012: Mechanism selector does not annotate comprehensions inside return statements
- **Discovered**: 2026-03-14, during matrix/max_plus trait update
- **Category**: Codegen
- **Severity**: Medium
- **Description**: `select_node` in `parallel.c` had no cases for `NODE_RETURN`, `NODE_LET`, `NODE_VAR`, `NODE_ASSIGN`, or `NODE_CALL`. These fell to `default: break` without recursing into child expressions. Comprehensions and for-loops nested inside return values, let/var bindings, assignments, or call arguments were never visited by the mechanism selector.
- **Fix**: Added cases for `NODE_RETURN` (recurse into value), `NODE_LET`/`NODE_VAR` (recurse into binding value), `NODE_ASSIGN` (recurse into assigned value), and `NODE_CALL` (recurse into all args).
- **Files**: `src/parallel.c` (`select_node` function)
- **Status**: Fixed (2026-03-14)

---

### BUG-013: C emitter generates thunk evaluators for uncalled algebra functions
- **Discovered**: 2026-03-14, during matmul performance investigation
- **Category**: Codegen
- **Severity**: High
- **Description**: The C emitter assigned thunk IDs and generated evaluator/constructor wrappers for ALL user-defined functions in imported algebras, regardless of whether they were called from any `use` block. With 9 functions (7 uncalled), the dead evaluator bodies (~1600 bytes) shifted the hot loop in `sigil_dot` to an M1 Firestorm fetch-block alignment that halved decode width, causing a 3.3x runtime slowdown (62s → 210s for N=750 matmul).
- **Fix**: Added `collect_call_names` helper to compute the transitive call closure from `use` block statements. Both the thunk ID assignment loop and the evaluator emission loop (Pass 3) now skip functions not in this closure. A `visited` set prevents infinite recursion from self-referential function bodies. Thunk count dropped from 9 to 4, restoring performance to 62s.
- **Files**: `src/c_emitter.c` (`collect_call_names`, thunk ID assignment, Pass 3 emission)
- **Status**: Fixed (2026-03-14)

---

### BUG-015: algebra_registry_add does not initialize distributive_count and distributives
- **Discovered**: 2026-03-14, during maxplus_test.sigil development
- **Category**: Codegen
- **Severity**: Critical
- **Description**: `algebra_registry_add` in `algebra.c` did not initialize `is_pure`, `distributives`, or `distributive_count`. Arena allocation does not zero memory, so these fields contained garbage from previously used heap blocks. With larger programs, the garbage `distributive_count` triggered a massive realloc in `algebra_register_declarations`, crashing with "pointer being freed was not allocated."
- **Fix**: Added `e->is_pure = false; e->distributives = NULL; e->distributive_count = 0;` to `algebra_registry_add`.
- **Files**: `src/algebra.c`
- **Status**: Fixed (2026-03-14)

---
