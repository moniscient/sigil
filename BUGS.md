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

---

---

## Resolved Bugs (moved to BUGS-RESOLVED.md when fixed)

---

Next valid BUG ID: BUG-014
