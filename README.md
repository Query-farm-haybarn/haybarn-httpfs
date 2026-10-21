# DuckDB HTTPFS extension

The httpfs extension is an autoloadable extension implementing a file system that allows reading remote/writing remote files. For plain HTTP(S), only file reading is supported. For object storage using the S3 API, the httpfs extension supports reading/writing/globbing files.

## Building & Loading the Extension

The DuckDB submodule must be initialized prior to building.
```bash
git submodule init
git pull --recurse-submodules
```

To build, type:
```
make setup-vcpkg
VCPKG_TOOLCHAIN_PATH=$pwd/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja make
```
Consider adding `GEN=ninja` and having `ccache` installed to speed up recompilations.

### VCPKG
`vcpkg`, a package manager for C++, it's highly reccomended to generate reproducible and stable builds, in particular here it serves to build the `openssl` and `curl` dependencies.
Without the `VCPKG_TOOLCHAIN_PATH` option, locally available libraries will be used from default search paths.

## Running
The resulting binary, that will also statically link and load the `httfps`, it's available like:
```
./build/release/duckdb
```
```sql
FROM read_blob('https://duckdb.org/');
```

## Testing
Some tests querying remote resources can be run already without further setup:
```
./build/release/test/unittest
```
Further integration testing uses a local MinIO setup using Docker. See the [testing documentation for more information on how to set this up locally](test).

## Response-side compression (`Accept-Encoding`)

The default `curl` client transparently handles compressed HTTP responses
(gzip, deflate, brotli, zstd — whichever encodings the bundled libcurl was
built with) for API-style requests, and asks for byte-exact responses for
everything that reads a file.

| Method                          | `Accept-Encoding` | Why |
|---------------------------------|-------------------|-----|
| `GET` (range and full)          | `identity`        | `Content-Range` applies to the encoded sequence (RFC 9110 §14.1.2), and a full `GET`'s headers become the file's metadata (size, ETag) before its body arrives. |
| `HEAD`                          | `identity`        | The response `Content-Length` is used as the file size. |
| `POST` / `PUT` / `DELETE` / `OPTIONS` | negotiate   | Outside the file-read lifecycle; S3 multipart-complete XML and HuggingFace JSON compress well. Curl decodes, and the response's `Content-Length` is rewritten to the decoded size. |

Caller-supplied `Accept-Encoding` headers are always dropped; the client owns
this policy. Identity requests are never decoded: an object stored with a
`Content-Encoding` (e.g. an S3 object uploaded with `Content-Encoding: gzip`)
is read as the bytes it was stored as, as before.

The optional `httplib` client (`SET httpfs_client_implementation = 'httplib'`)
sends `Accept-Encoding: identity` on every request; use the `curl` client for
response compression.

## Conditional reads (`If-Match` / `If-Unmodified-Since`)

To detect a remote file being rewritten mid-read (e.g. between a parquet
footer read and its row-group reads), reads of an open file carry an HTTP
precondition:

- **Strong ETag known** → `If-Match: <etag>` (RFC 9110 §13.1.1).
- **Only a weak ETag or a Last-Modified known** → `If-Unmodified-Since: <date>`
  (RFC 9110 §13.1.4). `If-Match` requires strong comparison, which a weak ETag
  never satisfies, so the date-based precondition is used instead.

A server that honors the precondition answers `412 Precondition Failed`, which
surfaces as a "changed after it was opened" error instead of stale or mixed
bytes; servers that ignore it answer as usual. S3 reads keep validating the
response ETag (and GCS reads use `x-goog-if-generation-match`).
`SET unsafe_disable_etag_checks = true;` sends no precondition.

## HTTP/2

The `curl` client negotiates HTTP/2 over TLS (ALPN) and falls back to
HTTP/1.1 when the server does not offer it. Parallel reads from the same
host are multiplexed as streams over one connection through a shared
`curl_multi` dispatcher.

| Setting               | Default | Meaning |
|-----------------------|---------|---------|
| `http_version`        | `auto`  | `auto` (HTTP/2 with HTTP/1.1 fallback), `1.1`, or `2.0` (fail if the peer refuses). |
| `http2_multiplex`     | `true`  | Route transfers through the shared dispatcher so parallel reads share one HTTP/2 connection. |
| `httpfs_curl_verbose` | `false` | Print curl's protocol log (TLS, ALPN, HTTP/2 frames) to stderr. |

## Cancellation

Interrupting a query (Ctrl-C, `con.interrupt()`) aborts its in-flight HTTP
requests instead of waiting for them to finish or time out. The `curl`
client checks for cancellation throughout every transfer (connect, upload,
waiting for the response, download) for all methods; the `httplib` client
can only stop while response bytes arrive. A cancelled request is never
retried.
