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
built with) on every method except `HEAD` and Range `GET`. This benefits
API workloads that exchange compressible payloads (S3 LIST XML, HuggingFace
JSON, multipart-complete responses) with gzip- or zstd-capable endpoints.

Per-method policy:

| Method            | `Accept-Encoding`     | Why                                                                                                  |
|-------------------|-----------------------|------------------------------------------------------------------------------------------------------|
| Range `GET`       | `identity`            | RFC 9110 §14.1.2: `Content-Range` applies to the encoded sequence, so encoded Range bytes don't match the object. |
| `HEAD`            | `identity`            | The response `Content-Length` is used as the file size; an encoded HEAD reports the wire size, not the object size. |
| Non-Range `GET`   | negotiate             | API calls and full-file fallback reads. Curl decodes transparently.                                  |
| `POST` / `PUT` / `DELETE` | negotiate     | Outside the file-read lifecycle. Response bodies (S3 multipart complete, etc.) benefit from compression. |

Caller-supplied `Accept-Encoding` headers are always stripped — the curl client
owns this policy. Range and HEAD responses are validated to be identity-encoded;
any non-identity `Content-Encoding` is rejected with a clear error rather than
delivered to the caller.

The optional `httplib` client (`SET httpfs_client_implementation = 'httplib'`)
is **identity-only on every request**. The bundled `duckdb_httplib` does not
reliably decode brotli/zstd, so the httplib path forces
`Accept-Encoding: identity` and rejects any non-identity `Content-Encoding`
response. Workloads that want response compression should use the default
`curl` client.

## Conditional reads (`If-Match` / `If-Unmodified-Since`)

To detect mid-read mutations of remote files (e.g. an S3 object rewritten
between the parquet footer read and the row-group reads), the curl client
sends an HTTP precondition on every request that follows a successful HEAD
or GET on the same file handle:

- **Strong ETag cached** → `If-Match: <etag>` (RFC 9110 §13.1.1).
- **Only a weak ETag or Last-Modified cached** → `If-Unmodified-Since: <date>`
  (RFC 9110 §13.1.4). RFC 9110 §13.1.1 requires strong comparison for
  `If-Match`, which weak ETags never satisfy, so we use the date-based
  precondition instead.

If the precondition fails the server returns `412 Precondition Failed`, which
the client surfaces as a clear "remote file changed" error rather than feeding
stale-or-corrupt bytes to the parquet/CSV reader. Server-side enforcement
replaces the previous client-side ETag string comparison, which had a long
tail of representation-mismatch bugs (weak vs strong ETag flavors, gzip vs
identity ETag variants, quoting inconsistencies across S3-compatible vendors).

Servers that don't honor preconditions on GetObject silently ignore the headers
and return `200`/`206` as usual. Range support is detected independently via
the existing `Content-Length` vs request-length check, which falls back to a
full-file download when the server doesn't speak Range.

`SET unsafe_disable_etag_checks = true;` bypasses precondition enforcement
entirely — the client sends no `If-Match` / `If-Unmodified-Since` and trusts
whatever the server returns.
