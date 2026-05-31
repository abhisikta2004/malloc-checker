#!/usr/bin/env bash
# Start the malloc-checker web UI
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ ! -f "$ROOT/build/MallocCheckerPlugin.dylib" ]; then
  echo "Plugin not built. Building…"
  mkdir -p build
  CLANG_CAND="$(ls /private/tmp/llvm-*/llvm-project*/llvm/build/bin/clang 2>/dev/null | head -1 || true)"
  if [ -n "$CLANG_CAND" ]; then
    LLVM_BUILD="$(dirname "$(dirname "$CLANG_CAND")")"
    cmake -S . -B build \
      -DLLVM_DIR="$LLVM_BUILD/lib/cmake/llvm" \
      -DClang_DIR="$LLVM_BUILD/lib/cmake/clang" \
      -DCMAKE_C_COMPILER="$CLANG_CAND" \
      -DCMAKE_CXX_COMPILER="${CLANG_CAND}++"
  else
    cmake -S . -B build \
      -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
      -DClang_DIR="$(brew --prefix llvm)/lib/cmake/clang" \
      -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
      -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
  fi
  cmake --build build
fi

export CLANG="${CLANG:-$(ls /private/tmp/llvm-*/llvm-project*/llvm/build/bin/clang 2>/dev/null | head -1 || echo "$(brew --prefix llvm 2>/dev/null)/bin/clang")}"
export PORT="${PORT:-8765}"

# If our server is already up, don't fail — just open the URL.
if curl -sf "http://127.0.0.1:${PORT}/api/status" >/dev/null 2>&1; then
  echo "Server already running at http://127.0.0.1:${PORT}"
  if [ -d "/Applications/Google Chrome.app" ]; then
    open -a "Google Chrome" "http://127.0.0.1:${PORT}/"
  else
    open "http://127.0.0.1:${PORT}/"
  fi
  exit 0
fi

# Port taken by something else — try the next few ports.
if lsof -i ":${PORT}" >/dev/null 2>&1; then
  for try in $(seq $((PORT + 1)) $((PORT + 10))); do
    if ! lsof -i ":${try}" >/dev/null 2>&1; then
      echo "Port ${PORT} busy; using ${try} instead."
      export PORT="${try}"
      break
    fi
  done
fi

echo "Open http://127.0.0.1:${PORT} in your browser"

# Launch Chrome if available (macOS).
if [ "$(uname -s)" = "Darwin" ]; then
  if [ -d "/Applications/Google Chrome.app" ]; then
    (sleep 1 && open -a "Google Chrome" "http://127.0.0.1:${PORT}/") &
  else
    (sleep 1 && open "http://127.0.0.1:${PORT}/") &
  fi
fi

exec python3 "$ROOT/web/server.py"
