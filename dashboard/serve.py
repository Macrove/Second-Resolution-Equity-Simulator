#!/usr/bin/env python3
"""Serves the dashboard page and the run log that ./sim appends to.

  python3 dashboard/serve.py [--log runs.jsonl] [--port 8000]

The page polls /runs.jsonl every 2 seconds, so a finished run shows up on its own.
"""
import argparse
import http.server
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="runs.jsonl")
    ap.add_argument("--port", type=int, default=8000)
    a = ap.parse_args()
    log_path = os.path.abspath(a.log)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                self.send(os.path.join(HERE, "index.html"), "text/html; charset=utf-8")
            elif path == "/runs.jsonl":
                self.send(log_path, "text/plain; charset=utf-8", missing_ok=True)
            else:
                self.send_error(404)

        def send(self, file, ctype, missing_ok=False):
            try:
                with open(file, "rb") as f:
                    body = f.read()
            except FileNotFoundError:
                if not missing_ok:
                    return self.send_error(404)
                body = b""
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"dashboard on http://127.0.0.1:{a.port}/  (log: {log_path})")
    server.serve_forever()


if __name__ == "__main__":
    main()
