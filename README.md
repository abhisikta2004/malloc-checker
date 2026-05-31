# malloc-checker

A Clang AST plugin that detects unchecked return values from heap allocators (`malloc`, `calloc`, `realloc`, `aligned_alloc`, `_aligned_malloc`, and custom allocators). It helps catch potential null-pointer dereferences before they become runtime crashes.

## Why it matters

Heap allocation can fail. Using an unchecked pointer is a common source of segfaults and security issues. This checker flags allocations that are dereferenced or passed to unsafe APIs without an intervening null check.

## Build

Requires **LLVM/Clang development libraries** (LLVM 14–17) and CMake 3.16+.

```bash
# macOS (Homebrew)
brew install llvm

# Configure and build
cd malloc-checker
mkdir build && cd build
cmake .. \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DClang_DIR="$(brew --prefix llvm)/lib/cmake/clang" \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang"
cmake --build .
```

The plugin is built as `MallocCheckerPlugin.dylib` (macOS) or `MallocCheckerPlugin.so` (Linux).

## Usage

```bash
clang -isysroot $(xcrun --show-sdk-path) \
  -fplugin=./build/MallocCheckerPlugin.dylib \
  -Xclang -plugin -Xclang malloc-checker \
  -fsyntax-only myfile.c
```

### Plugin arguments

| Argument | Description |
|----------|-------------|
| `-fplugin-arg-malloc-checker-stats` | Print per-TU statistics |
| `-fplugin-arg-malloc-checker-allocator=name` | Add a custom allocator to check |
| `-fplugin-arg-malloc-checker-report=path.json` | Write JSON warning report |
| `-fplugin-arg-malloc-checker-Wno-malloc-check` | Suppress all checker warnings |

Example with custom allocator and stats:

```bash
clang -isysroot $(xcrun --show-sdk-path) \
  -fplugin=./build/MallocCheckerPlugin.dylib \
  -Xclang -plugin -Xclang malloc-checker \
  -fplugin-arg-malloc-checker-allocator=my_pool_alloc \
  -fplugin-arg-malloc-checker-stats \
  -fsyntax-only myfile.c
```

## Supported null-check patterns

### 1. Direct if-null check
```c
int *p = malloc(n);
if (p == NULL) { return; }
*p = 1;  // OK
```

### 2. Logical-not guard
```c
int *p = malloc(n);
if (!p) return;
*p = 1;  // OK
```

### 3. assert
```c
int *p = malloc(n);
assert(p);
*p = 1;  // OK
```

### 4. Ternary guard
```c
int *p = malloc(n);
return p ? p : NULL;  // OK — not dereferenced here
```

### 5. Deferred check (CFG-aware)
```c
int *p = malloc(n);
do_something();
if (p) use(p);  // OK when check dominates use
```

### 6. Early return
```c
int *p = malloc(n);
if (!p) return -1;
*p = 1;  // OK
```

### 7. Casts are transparent
```c
int *p = (int *)malloc(n);
if (!p) return;
*p = 1;  // OK
```

## False-positive suppression

No warning when:

- `(void)malloc(n)` — intentional discard
- Call is inside `static_assert` / `_Static_assert`
- Variable has `__attribute__((unused))`
- Result is returned directly (pass-through to caller)
- Pointer is passed to another function before use (escape — callee may check)

## Running tests

```bash
cd build
cmake --build . --target check
```

Or manually:

```bash
bash tests/run_tests.sh ./build/MallocCheckerPlugin.dylib $(brew --prefix llvm)/bin/clang
```

## Web UI

A browser-based output viewer runs the checker and displays warnings:

```bash
bash web/start.sh
```

Then open **http://127.0.0.1:8765**

- Pick a sample file (`test_unchecked.c`, `test_checked.c`, `test_edge_cases.c`) or paste your own C code
- Click **Run checker** to see warnings, stats, and raw clang output

Or start the server manually:

```bash
export CLANG="/path/to/llvm/bin/clang"
python3 web/server.py
```

## Limitations

- **Intra-procedural only** — does not analyze callees for null checks
- **No aliasing analysis** — pointer copies may hide or duplicate warnings
- **CFG precision** — complex control flow may still produce false positives/negatives
- **Custom allocators** must be registered via `-allocator=` plugin argument

## Diagnostic output

Warnings use the message:

```
warning: return value of 'malloc' is not checked for NULL — potential null dereference
```

When deferred tracking finds an unsafe dereference first, a note points to the dereference site and a fix-it hint suggests:

```c
if (!ptr) { /* handle error */ return; }
```

## License

Provided for educational / lab use.
