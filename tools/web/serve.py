#!/usr/bin/env python3
"""Serve a local browser build. Game files are not on this server."""

import argparse
import functools
import http.server
import os
from pathlib import Path
from urllib.parse import urlsplit


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404, "Directory listing disabled")
        return None

    def translate_path(self, path):
        translated = Path(super().translate_path(path)).resolve()
        root = Path(self.directory).resolve()
        if not translated.is_relative_to(root):
            return str(root / "__outside__")
        return str(translated)

    def send_head(self):
        path = Path(self.translate_path(self.path))
        root = Path(self.directory).resolve()
        if not path.is_relative_to(root):
            self.send_error(403, "Path outside build directory")
            return None
        if path.is_dir():
            index = path / "broken_bond.html"
            if not index.is_file() or not index.resolve().is_relative_to(root):
                self.send_error(404, "File not found")
                return None
            if not urlsplit(self.path).path.endswith("/"):
                return super().send_head()
            path = index
        try:
            stream = path.open("rb")
        except OSError:
            self.send_error(404, "File not found")
            return None
        stat = os.fstat(stream.fileno())
        self.send_response(200)
        self.send_header("Content-Type", self.guess_type(str(path)))
        self.send_header("Content-Length", str(stat.st_size))
        self.send_header("Last-Modified", self.date_time_string(stat.st_mtime))
        self.end_headers()
        return stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", default="out/build/web-wasm64-release/web")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    if args.bind not in ("127.0.0.1", "localhost", "::1"):
        parser.error("refusing a non-loopback address; this build stays on this machine")
    directory = Path(args.directory).resolve()
    if not directory.is_dir():
        parser.error(f"build directory does not exist: {directory}")
    handler = functools.partial(Handler, directory=str(directory))
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f"Serving {directory}", flush=True)
        print(f"Open http://{args.bind}:{server.server_port}/", flush=True)
        print("Local only. The page loads your game from this browser, not from the server.", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
