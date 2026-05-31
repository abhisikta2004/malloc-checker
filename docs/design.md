# malloc-checker Design

## Overview

`malloc-checker` is a Clang frontend plugin that walks the AST of each translation unit, identifies calls to heap allocators, and verifies that their return values are null-checked before unsafe use.

## Architecture

```
PluginASTAction (MallocCheckerAction)
    └── ASTConsumer (MallocCheckerConsumer)
            └── RecursiveASTVisitor (MallocCheckerVisitor)
                    ├── VisitCallExpr      — detect allocator calls
                    └── TraverseFunctionDecl — per-function CFG analysis
```

### Components

1. **MallocCheckerAction** — Registers the plugin, parses `-plugin-arg-malloc-checker` options (`-stats`, `-allocator=`, `-report=`, `-Wno-malloc-check`).

2. **MallocCheckerVisitor** — Core analysis:
   - Visits every `CallExpr` in each function body
   - Identifies allocator calls by function name (configurable set)
   - Applies immediate suppression rules (void discard, static_assert, pass-through return, unused attribute)
   - Records `VarDecl` assignments for deferred CFG analysis

3. **CFG analysis** — For each tracked variable:
   - Builds a `clang::CFG` via `CFG::buildCFG()`
   - Walks basic blocks in order, tracking null-check state
   - Detects dominating null checks (`if (!p) return`, `if (p == NULL)`, `assert(p)`)
   - Emits warnings at the first unsafe dereference (not at the malloc site) for better actionability

4. **Escape analysis** — If a tracked pointer is passed as a function argument or returned before dereference, analysis stops (callee/caller may check).

## Null-check detection

| Pattern | Detection method |
|---------|-------------------|
| `if (p == NULL)` | `BinaryOperator` BO_EQ/BO_NE with null constant |
| `if (!p)` | `UnaryOperator` UO_LNot on `DeclRefExpr` |
| `assert(p)` | Callee name `assert`, arg references var |
| Ternary guard | Parent is `ConditionalOperator` |
| Early return | `IfStmt` with null check + `ReturnStmt` in then/else |

## Diagnostics

- **Warning**: unchecked allocator return / unsafe dereference
- **Note**: allocation site, dereference site, fix-it hint
- **Fix-it**: `FixItHint::CreateInsertion` after the allocation statement
- **JSON report**: optional `-report=` output with line, column, function, allocator, message

## Statistics

At end of TU (with `-stats`):

```
[malloc-checker] TU: file.c — N malloc calls checked, W warnings emitted, C clean
```

## Known limitations

- No inter-procedural analysis
- No pointer alias tracking
- Member assignments (`obj.field = malloc(...)`) use dereference-site analysis only
- Custom diagnostic group `-Wno-malloc-check` is handled via plugin argument (full compiler `-W` group integration requires TableGen diagnostics)

## File map

| File | Purpose |
|------|---------|
| `plugin/MallocChecker.h` | Shared types, action/consumer declarations |
| `plugin/MallocChecker.cpp` | Plugin implementation |
| `tests/*.c` | Positive/negative test cases |
| `tests/run_tests.sh` | Test harness |
