# Bug Tracker

## Bug Reporting Protocol

**Mandatory**: When encountering any unexpected behavior, test failure, or anomaly during development—even if worked around or tangential to the current task—immediately append a bug report here before continuing. Never assume a bug will be "remembered" for later.

### Workflow
1. **New bugs**: Append to the bottom of this file, above the "Next valid BUG ID" line. Use the ID shown there, then increment it.
2. **Resolved bugs**: When a bug is fixed, update its Status to "Fixed (date)" and move the entire entry to `BUGS-RESOLVED.md`. Do not reuse its BUG ID number.
3. **Next BUG ID**: The last line of this file always shows the next valid BUG ID. Update it after each insertion.

### Bug Categories
- **Parser**: Grammar issues, parse failures
- **Semantic**: Type checking, binding resolution, trait matching
- **Codegen**: C emission, type registry issues
- **Runtime**: Task scheduler, channel operations, coroutine behavior
- **GC**: Garbage collector, handle table, shadow stack issues
- **Stdlib**: Standard library functions, posix module

### Severity Levels
- **Critical**: Crashes, data corruption, security issues
- **High**: Correctness bugs, wrong output
- **Medium**: Performance issues, edge cases
- **Low**: Minor issues, cosmetic problems

---

## Open Bugs

### BUG-011: Postfix sigil desugaring inside algebra fn body produces wrong call target
- **Discovered**: 2026-03-10, during Collatz algebra implementation
- **Category**: Codegen
- **Severity**: Medium
- **Reproduction**: Define a postfix sigil fn `fn step int n ~> returns int` inside an algebra. In another fn body within the same algebra, use `do n ~> end` where `n` is a local variable. The desugarer produces `n(step())` instead of `step(n)`.
- **Observed**: `sigil_n(thunk_alloc(...))` in generated C — treats variable name `n` as the fn call target
- **Expected**: `sigil_step(n)` — the fn name `step` should be the call target with `n` as the argument
- **Hypothesis**: The postfix desugaring path confuses the left operand (variable `n`) with the fn name when the expression appears inside an algebra fn body rather than a `use` block
- **Files**: `src/desugarer.c` (precedence climbing / postfix handling)
- **Status**: Open

### BUG-014: Dense integer-keyed maps should be lowered to contiguous arrays in codegen
- **Discovered**: 2026-03-14, during AMX matrix multiply optimization
- **Category**: Codegen
- **Severity**: Low (optimization, not a correctness bug)
- **Description**: Sigil's only collection primitive is `map`, backed by hash tables. When a map has dense integer keys 0..N-1 (constructed by `range`, `collect`, sequential `set`, or `row`/`matrix` constructors), every element access pays hash computation + bucket traversal (~50-200ns) for what should be a direct array index (~1-5ns). This is the dominant performance cost in most Sigil programs — the AMX matrix multiply achieved 950x speedup largely by extracting SigilMap data into contiguous `double[]` before computing.
- **Proposed fix**: Add a compilation pass after type checking that detects maps with provably dense integer keys and emits `int64_t[]` (or `double[]`, `SigilVal[]`) instead of `SigilMap*` in the generated C. Detection criteria: map constructed by `collect from i in range`, sequential `set` in a for-loop, `mapnew` followed only by sequential integer-keyed `set` calls, or `row`/`matrix` constructors. When lowered, `get`/`set` emit direct array indexing, `mapcount`/`length` emit the stored size, and `for k in m` emits a simple counted loop. Maps that escape to unknown contexts or receive non-sequential keys fall back to `SigilMap*`.
- **Impact**: Would eliminate extraction/reconstruction overhead in AMX/SIMD paths, make all comprehension strategies faster, and bring sequential Sigil performance close to C for array-oriented code.
- **Files**: `src/c_emitter.c` (type inference + emission), potentially a new `src/array_lowering.c` pass
- **Status**: Open (optimization — low priority)

---

## Resolved Bugs (moved to BUGS-RESOLVED.md when fixed)

---

Next valid BUG ID: BUG-015
