#!/usr/bin/env python3
"""Local web server for malloc-checker — runs the plugin and returns JSON output."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "build" / "MallocCheckerPlugin.dylib"
TESTS = ROOT / "tests"
WEB = Path(__file__).resolve().parent

TEMP_CLANG = Path(
    "/private/tmp/llvm-20260531-48758-f18gbb/llvm-project-22.1.6.src/llvm/build/bin/clang"
)


def find_clang() -> str | None:
    if env := os.environ.get("CLANG"):
        if Path(env).is_file():
            return env
    brew = shutil.which("brew")
    if brew:
        try:
            prefix = subprocess.check_output([brew, "--prefix", "llvm"], text=True).strip()
            candidate = Path(prefix) / "bin" / "clang"
            if candidate.is_file():
                return str(candidate)
        except subprocess.CalledProcessError:
            pass
    if TEMP_CLANG.is_file():
        return str(TEMP_CLANG)
    return shutil.which("clang")


def find_sdk() -> str:
    if sdk := os.environ.get("SDK"):
        return sdk
    try:
        out = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        if out:
            return out
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return ""


def parse_diagnostics(output: str) -> list[dict]:
    warnings: list[dict] = []
    warning_re = re.compile(
        r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): "
        r"(?P<kind>warning|note|error): (?P<message>.+)$"
    )
    for line in output.splitlines():
        m = warning_re.match(line.strip())
        if m and m.group("kind") == "warning":
            warnings.append(
                {
                    "file": m.group("file"),
                    "line": int(m.group("line")),
                    "col": int(m.group("col")),
                    "message": m.group("message"),
                }
            )
    return warnings


def run_checker(source: str, filename: str, stats: bool) -> dict:
    clang = find_clang()
    if not clang:
        return {"ok": False, "error": "LLVM clang not found. Set CLANG or run: brew install llvm"}
    if not PLUGIN.is_file():
        return {
            "ok": False,
            "error": f"Plugin not built. Run: cd build && cmake --build . ({PLUGIN})",
        }

    sdk = find_sdk()
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".c", prefix="malloc_check_", delete=False
    ) as tmp:
        tmp.write(source)
        tmp_path = tmp.name

    cmd = [
        clang,
        "-fsyntax-only",
        f"-fplugin={PLUGIN}",
        "-Xclang",
        "-plugin",
        "-Xclang",
        "malloc-checker",
    ]
    if sdk:
        cmd.extend(["-isysroot", sdk])
    if stats:
        cmd.append("-fplugin-arg-malloc-checker-stats")
    cmd.append(tmp_path)

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    finally:
        Path(tmp_path).unlink(missing_ok=True)

    combined = (proc.stdout + proc.stderr).strip()
    warnings = parse_diagnostics(combined)
    stats_line = ""
    for line in combined.splitlines():
        if line.startswith("[malloc-checker]"):
            stats_line = line
            break

    return {
        "ok": True,
        "filename": filename,
        "exit_code": proc.returncode,
        "output": combined,
        "warnings": warnings,
        "warning_count": len(warnings),
        "stats": stats_line,
        "clean": len(warnings) == 0,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        print(f"[web] {self.address_string()} {fmt % args}")

    def _send_json(self, data: dict, status: int = 200) -> None:
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path: Path, content_type: str) -> None:
        if not path.is_file():
            self.send_error(404)
            return
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path in ("/", "/index.html"):
            self._send_file(WEB / "index.html", "text/html; charset=utf-8")
            return
        if parsed.path == "/style.css":
            self._send_file(WEB / "style.css", "text/css; charset=utf-8")
            return
        if parsed.path == "/api/status":
            self._send_json(
                {
                    "clang": find_clang(),
                    "plugin": str(PLUGIN),
                    "plugin_exists": PLUGIN.is_file(),
                    "sdk": find_sdk(),
                }
            )
            return
        if parsed.path == "/api/samples":
            samples = []
            for name in ("test_unchecked.c", "test_checked.c", "test_edge_cases.c", "test_malloc_unsafe.c", "test_malloc_safe.c"):
                path = TESTS / name
                if path.is_file():
                    samples.append({"name": name, "source": path.read_text()})
            self._send_json({"samples": samples})
            return
        self.send_error(404)

    def do_POST(self) -> None:
        if urlparse(self.path).path != "/api/check":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except json.JSONDecodeError:
            self._send_json({"ok": False, "error": "Invalid JSON body"}, 400)
            return

        source = payload.get("source", "")
        filename = payload.get("filename", "input.c")
        stats = bool(payload.get("stats", True))
        if not source.strip():
            self._send_json({"ok": False, "error": "Empty source code"}, 400)
            return

        self._send_json(run_checker(source, filename, stats))


def main() -> None:
    port = int(os.environ.get("PORT", "8765"))
    host = os.environ.get("HOST", "127.0.0.1")
    print(f"malloc-checker web UI: http://{host}:{port}")
    print(f"  plugin: {PLUGIN} ({'found' if PLUGIN.is_file() else 'MISSING — build first'})")
    print(f"  clang:  {find_clang() or 'NOT FOUND'}")
    HTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    main()
