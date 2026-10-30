#!/usr/bin/env python3
"""MiniPi TLS broker (M2 boundary). The MIT-only minipi binary talks plaintext
HTTP to this localhost broker; the broker performs TLS via the OS/Python trust
store and forwards to the upstream LLM API. The API key NEVER leaves this
process's environment — it is injected as an Authorization header here only.

Security properties:
  * binds 127.0.0.1 only (never exposed on the network)
  * allow-list of upstream hosts (default: integrate.api.nvidia.com, api.openai.com)
  * strips any client-supplied Authorization before injecting its own
  * no request/response logging of bodies or headers
Run:  MINIPI_BROKER_PORT=8777 python3 tools/tls_broker.py
"""
import json, os, ssl, sys, urllib.request, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ALLOWED_HOSTS = set(
    h.strip() for h in os.environ.get(
        "MINIPI_BROKER_ALLOWED_HOSTS",
        "integrate.api.nvidia.com,api.openai.com,api.anthropic.com").split(",") if h.strip())
KEY_ENV = {"integrate.api.nvidia.com": "NVIDIA_API_KEY",
           "api.openai.com": "OPENAI_API_KEY",
           "api.anthropic.com": "ANTHROPIC_API_KEY"}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass  # never log requests

    def _reply(self, code, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > 8 * 1024 * 1024:
                return self._reply(400, b'{"error":"bad length"}')
            raw = self.rfile.read(n)
            p = self.path
            # accept both absolute-form ("http://host/path") and origin-form ("/host/path")
            if p.startswith("http://127.0.0.1") or p.startswith("http://localhost"):
                q = urllib.parse.urlparse(p)
                path = q.path
            else:
                path = p
            parts = path.lstrip("/").split("/", 1)
            host = parts[0].split(":")[0] if parts else ""
            upath = "/" + (parts[1] if len(parts) > 1 else "")
            if host not in ALLOWED_HOSTS:
                return self._reply(403, json.dumps({"error": "host not allowed", "got": host}).encode())
            key = os.environ.get(KEY_ENV.get(host, ""))
            if not key:
                return self._reply(500, b'{"error":"broker missing api key env"}')
            req = urllib.request.Request(
                "https://" + host + upath, data=raw, method="POST",
                headers={"Content-Type": "application/json",
                         "Accept": "text/event-stream",
                         "Authorization": "Bearer " + key})
            with urllib.request.urlopen(req, timeout=300,
                                        context=ssl.create_default_context()) as r:
                self._reply(r.status, r.read())
        except urllib.error.HTTPError as e:
            self._reply(e.code, e.read())
        except Exception as e:
            self._reply(502, json.dumps({"error": type(e).__name__}).encode())


if __name__ == "__main__":
    port = int(os.environ.get("MINIPI_BROKER_PORT", "8777"))
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"[broker] listening on http://127.0.0.1:{port}", file=sys.stderr)
    srv.serve_forever()
