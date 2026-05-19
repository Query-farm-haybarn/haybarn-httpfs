#!/usr/bin/env python3
"""
HTTP server used by httpfs integration tests. Serves files from a base
directory under a few specialized paths that exercise distinct corners of
the response-side compression and conditional-read behavior.

Compression endpoints
---------------------

  GET /identity/<filename>
      Returns the file as-is with Content-Encoding: identity. Baseline.

  GET /gzip/<filename>
      Returns the file gzip-compressed with Content-Encoding: gzip on any
      response (including Range responses where it's a contract violation).

  GET /range_misbehaved/<filename>
      Returns the FULL file gzip-compressed with Content-Encoding: gzip,
      even when the request carries a Range header. Verifies the safety
      contract: clients must refuse compressed bytes on Range responses.

  GET /range_ok/<filename>
      Honors Range requests correctly (identity bytes for the requested
      byte range), full file uncompressed otherwise.

Conditional-read endpoints
--------------------------

  GET /conditional/<filename>
      Returns the file with a strong ETag and Last-Modified header.
      Honors If-Range and If-Match per RFC 9110:
        - If-Range matches:   206 with the requested range.
        - If-Range mismatch:  200 with the full current representation.
        - If-Match  matches:  200/206 normally.
        - If-Match  mismatch: 412 Precondition Failed.
      The "current representation" mutates whenever a control request
      (POST /mutate/<filename>) is received — see below.

  GET /weak_etag/<filename>
      Same semantics as /conditional/, but the ETag is weak (W/"...").
      Servers SHOULD only honor strong validators for If-Range
      (RFC 9110 §13.1.5); this server REJECTS If-Range when the value is
      a weak ETag, simulating the strict behavior that forces the client
      to fall back to Last-Modified.

  POST /mutate/<filename>
      Control endpoint. Bumps an in-memory generation counter for the
      file, so subsequent reads of /conditional/<filename> and
      /weak_etag/<filename> return a different representation (different
      bytes, different ETag, different Last-Modified). Returns 204.

Environment variables:
  COMPRESSION_TEST_SERVER_DIR    base directory of files to serve
  COMPRESSION_TEST_SERVER_PORT   port to listen on (default: 8009)

This server intentionally avoids any third-party dependencies.
"""

import email.utils
import gzip
import hashlib
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


BASE_DIR = os.environ.get("COMPRESSION_TEST_SERVER_DIR")
PORT = int(os.environ.get("COMPRESSION_TEST_SERVER_PORT", "8009"))


# Per-file generation counter for the conditional-read endpoints. Bumped by
# POST /mutate/<filename>. The body served at each generation is
#   original_bytes + b" [generation N]"
# and the ETag is the hex SHA-256 of that body, with the strong/weak flavor
# determined by the endpoint.
_GENERATION_LOCK = threading.Lock()
_GENERATION = {}                # filename -> int
_GENERATION_MTIME = {}          # filename -> unix seconds for Last-Modified


def _current_generation(filename):
    with _GENERATION_LOCK:
        if filename not in _GENERATION:
            _GENERATION[filename] = 0
            _GENERATION_MTIME[filename] = time.time()
        return _GENERATION[filename], _GENERATION_MTIME[filename]


def _bump_generation(filename):
    with _GENERATION_LOCK:
        _GENERATION[filename] = _GENERATION.get(filename, 0) + 1
        # Bump mtime by at least one second so Last-Modified comparisons see
        # a strictly newer value (HTTP-date resolution is one second).
        prior = _GENERATION_MTIME.get(filename, 0)
        _GENERATION_MTIME[filename] = max(time.time(), prior + 1)


def _representation(filename, body):
    """Return (current_body, etag_opaque, http_date) for the conditional endpoints."""
    generation, mtime = _current_generation(filename)
    current_body = body + (" [generation %d]" % generation).encode("utf-8") if generation else body
    digest = hashlib.sha256(current_body).hexdigest()
    return current_body, digest, email.utils.formatdate(timeval=mtime, usegmt=True)


def _resolve(filename):
    if BASE_DIR is None:
        return None
    candidate = os.path.normpath(os.path.join(BASE_DIR, filename))
    if not candidate.startswith(os.path.normpath(BASE_DIR) + os.sep):
        return None
    if not os.path.isfile(candidate):
        return None
    return candidate


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # noqa: A002
        # quiet unless something goes wrong
        if args and isinstance(args[0], str) and args[0].startswith(("4", "5")):
            sys.stderr.write("%s - %s\n" % (self.address_string(), format % args))

    def _serve_bytes(self, payload, content_encoding=None, status=200, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Accept-Ranges", "bytes")
        if content_encoding is not None:
            self.send_header("Content-Encoding", content_encoding)
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)

    def _not_found(self):
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_HEAD(self):
        self._dispatch(head=True)

    def do_GET(self):
        self._dispatch(head=False)

    def do_POST(self):
        path = self.path
        if path.startswith("/mutate/"):
            filename = path[len("/mutate/"):]
            _bump_generation(filename)
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self._not_found()

    def _dispatch(self, head):
        path = self.path
        if path.startswith("/identity/"):
            self._handle_identity(path[len("/identity/"):], head)
        elif path.startswith("/gzip/"):
            self._handle_gzip(path[len("/gzip/"):], head)
        elif path.startswith("/range_misbehaved/"):
            self._handle_range_misbehaved(path[len("/range_misbehaved/"):], head)
        elif path.startswith("/range_ok/"):
            self._handle_range_ok(path[len("/range_ok/"):], head)
        elif path.startswith("/conditional/"):
            self._handle_conditional(path[len("/conditional/"):], head, weak_etag=False)
        elif path.startswith("/weak_etag/"):
            self._handle_conditional(path[len("/weak_etag/"):], head, weak_etag=True)
        elif path.startswith("/mutate/"):
            # GET-based mutate so SQL tests can trigger it via read_text. Generation is bumped
            # only on the actual body fetch (head=False), not on HEAD probes — read_text issues
            # both, and we want exactly one bump per logical read_text invocation.
            filename = path[len("/mutate/"):]
            if not head:
                _bump_generation(filename)
            payload = b"mutated\n"
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Content-Type", "text/plain")
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            if not head:
                self.wfile.write(payload)
        else:
            self._not_found()

    def _read_file(self, filename):
        resolved = _resolve(filename)
        if resolved is None:
            return None
        with open(resolved, "rb") as fp:
            return fp.read()

    def _handle_identity(self, filename, head):
        body = self._read_file(filename)
        if body is None:
            self._not_found()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Encoding", "identity")
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def _handle_gzip(self, filename, head):
        body = self._read_file(filename)
        if body is None:
            self._not_found()
            return
        compressed = gzip.compress(body)
        self.send_response(200)
        self.send_header("Content-Length", str(len(compressed)))
        self.send_header("Content-Encoding", "gzip")
        self.end_headers()
        if not head:
            self.wfile.write(compressed)

    def _handle_range_misbehaved(self, filename, head):
        # Returns the full file gzip-compressed even when a Range header is present.
        body = self._read_file(filename)
        if body is None:
            self._not_found()
            return
        compressed = gzip.compress(body)
        # Pretend to honor Range — actually return full compressed payload.
        status = 206 if self.headers.get("Range") else 200
        self.send_response(status)
        self.send_header("Content-Length", str(len(compressed)))
        self.send_header("Content-Encoding", "gzip")
        if self.headers.get("Range"):
            self.send_header("Content-Range", "bytes 0-%d/%d" % (len(body) - 1, len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(compressed)

    def _parse_range(self, range_header, body_len):
        """Parse 'bytes=START-END' or 'bytes=START-'; return (start, end) or None on error."""
        try:
            assert range_header.startswith("bytes=")
            spec = range_header[len("bytes="):]
            start_s, end_s = spec.split("-", 1)
            start = int(start_s)
            end = int(end_s) if end_s else body_len - 1
            if start < 0 or end >= body_len or start > end:
                return None
            return start, end
        except Exception:
            return None

    def _handle_conditional(self, filename, head, weak_etag):
        """Conditional-read endpoint. Honors If-Match (strong-comparison) and
        If-Unmodified-Since per RFC 9110 §13.1.1 / §13.1.4. Precondition failure
        on either header returns 412 Precondition Failed.

        With `weak_etag=True` the ETag is returned as W/"...". RFC 9110 §13.1.1
        requires strong comparison for If-Match, which weak ETags never satisfy —
        so a weak ETag in If-Match is rejected with 412 here. (In practice clients
        should send If-Unmodified-Since instead when only a weak ETag is cached.)
        """
        body = self._read_file(filename)
        if body is None:
            self._not_found()
            return
        current_body, etag_opaque, last_modified = _representation(filename, body)
        etag_value = ('W/"%s"' if weak_etag else '"%s"') % etag_opaque

        if_match = self.headers.get("If-Match")
        if_unmodified_since = self.headers.get("If-Unmodified-Since")
        range_header = self.headers.get("Range")

        # If-Match: strong-comparison required by RFC 9110 §13.1.1. A weak ETag in
        # If-Match (either the client sent W/"..." or our representation is weak)
        # never satisfies strong comparison.
        if if_match is not None:
            if_match_ok = (not weak_etag) and (not if_match.startswith("W/")) and (if_match == etag_value)
            if not if_match_ok:
                self.send_response(412)
                self.send_header("Content-Length", "0")
                self.send_header("ETag", etag_value)
                self.end_headers()
                return

        # If-Unmodified-Since: 412 if the resource HAS been modified since the
        # given date. We compare HTTP-date strings; equal-or-older means unchanged.
        if if_unmodified_since is not None and if_unmodified_since != last_modified:
            self.send_response(412)
            self.send_header("Content-Length", "0")
            self.send_header("Last-Modified", last_modified)
            self.end_headers()
            return

        if range_header:
            parsed = self._parse_range(range_header, len(current_body))
            if parsed is None:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % len(current_body))
                self.end_headers()
                return
            start, end = parsed
            chunk = current_body[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(current_body)))
            self.send_header("ETag", etag_value)
            self.send_header("Last-Modified", last_modified)
            self.send_header("Content-Encoding", "identity")
            self.end_headers()
            if not head:
                self.wfile.write(chunk)
            return

        # No Range header: full-body response.
        self.send_response(200)
        self.send_header("Content-Length", str(len(current_body)))
        self.send_header("ETag", etag_value)
        self.send_header("Last-Modified", last_modified)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Encoding", "identity")
        self.end_headers()
        if not head:
            self.wfile.write(current_body)

    def _handle_range_ok(self, filename, head):
        body = self._read_file(filename)
        if body is None:
            self._not_found()
            return
        range_header = self.headers.get("Range")
        if not range_header:
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Encoding", "identity")
            self.end_headers()
            if not head:
                self.wfile.write(body)
            return
        # Parse "bytes=START-END" or "bytes=START-"
        try:
            assert range_header.startswith("bytes=")
            spec = range_header[len("bytes="):]
            start_s, end_s = spec.split("-", 1)
            start = int(start_s)
            end = int(end_s) if end_s else len(body) - 1
            if start < 0 or end >= len(body) or start > end:
                raise ValueError
        except Exception:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % len(body))
            self.end_headers()
            return
        chunk = body[start:end + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(chunk)))
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(body)))
        self.send_header("Content-Encoding", "identity")
        self.end_headers()
        if not head:
            self.wfile.write(chunk)


def main():
    if BASE_DIR is None:
        sys.stderr.write("COMPRESSION_TEST_SERVER_DIR not set\n")
        sys.exit(1)
    if not os.path.isdir(BASE_DIR):
        sys.stderr.write("COMPRESSION_TEST_SERVER_DIR %r does not exist\n" % BASE_DIR)
        sys.exit(1)
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write("compression test server listening on 127.0.0.1:%d serving %s\n"
                     % (PORT, BASE_DIR))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
