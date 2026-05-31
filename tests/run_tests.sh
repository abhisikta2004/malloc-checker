#!/usr/bin/env bash
# Usage: run_tests.sh <plugin.so|.dylib> [clang]
set -eo pipefail

PLUGIN="${1:?Usage: run_tests.sh <plugin> [clang]}"
CLANG="${2:-clang}"
PASS=0
FAIL=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDK="${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null || true)}"
COMMON_FLAGS=(-fsyntax-only -fplugin="$PLUGIN" -Xclang -plugin -Xclang malloc-checker)
if [ -n "$SDK" ]; then
    COMMON_FLAGS+=(-isysroot "$SDK")
fi

run() {
    local file="$1"
    local expect_warn="$2"
    shift 2
    local extra_args=("$@")
    local output count

    output=$("$CLANG" "${COMMON_FLAGS[@]}" "${extra_args[@]}" "$file" 2>&1) || true
    count=$(echo "$output" | grep -c "warning:" || true)

    if [ "$expect_warn" = "any" ] && [ "$count" -gt 0 ]; then
        echo "PASS  $file ($count warnings)"
        PASS=$((PASS + 1))
    elif [ "$expect_warn" = "none" ] && [ "$count" -eq 0 ]; then
        echo "PASS  $file (clean)"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $file (expected $expect_warn, got $count warnings)"
        echo "$output"
        FAIL=$((FAIL + 1))
    fi
}

run "${ROOT_DIR}/tests/test_unchecked.c" any
run "${ROOT_DIR}/tests/test_checked.c" none
run "${ROOT_DIR}/tests/test_edge_cases.c" any

# TC-24: custom allocator via plugin arg
run "${ROOT_DIR}/tests/test_edge_cases.c" any \
    -fplugin-arg-malloc-checker-allocator=my_alloc

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
