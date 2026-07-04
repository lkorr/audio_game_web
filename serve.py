#!/usr/bin/env python3
# Local dev server that sends the COOP/COEP headers the audio worklet needs
# (SharedArrayBuffer). Plain `python -m http.server` does NOT set these, so the
# audio thread won't start under it -- use this instead.
#
#   python serve.py            # http://localhost:8000/
#   python serve.py 9000       # custom port
#
# Deploy to a real host with the equivalent headers (see _headers / README).

import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000

class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # WASM must be served with the right MIME type.
        super().end_headers()

    def guess_type(self, path):
        if path.endswith(".wasm"):
            return "application/wasm"
        return super().guess_type(path)

if __name__ == "__main__":
    print(f"Serving with COOP/COEP on http://localhost:{PORT}/  (Ctrl+C to stop)")
    ThreadingHTTPServer(("", PORT), Handler).serve_forever()
