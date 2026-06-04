#!/usr/bin/env bash
#Note: DONT run as root

# Haybarn: this fork ships the CLI as `haybarn` (DuckDB ships it as `duckdb` —
# see tools/shell/CMakeLists.txt OUTPUT_NAME). Prefer the haybarn binary, but
# keep the duckdb names as a fallback for upstream parity.
DUCKDB_PATH=haybarn
if command -v haybarn >/dev/null 2>&1; then
  DUCKDB_PATH=haybarn
elif test -f build/release/haybarn; then
  DUCKDB_PATH=build/release/haybarn
elif test -f build/reldebug/haybarn; then
  DUCKDB_PATH=build/reldebug/haybarn
elif test -f build/debug/haybarn; then
  DUCKDB_PATH=build/debug/haybarn
elif command -v duckdb >/dev/null 2>&1; then
  DUCKDB_PATH=duckdb
elif test -f build/release/duckdb; then
  DUCKDB_PATH=build/release/duckdb
elif test -f build/reldebug/duckdb; then
  DUCKDB_PATH=build/reldebug/duckdb
elif test -f build/debug/duckdb; then
  DUCKDB_PATH=build/debug/duckdb
fi

rm -rf test/test_data
mkdir -p test/test_data

generate_large_parquet_query=$(cat <<EOF

CALL DBGEN(sf=1);
COPY lineitem TO 'test/test_data/presigned-url-lineitem.parquet' (FORMAT 'parquet');

EOF
)
$DUCKDB_PATH -c "$generate_large_parquet_query"

# Generate Storage Version
$DUCKDB_PATH  test/test_data/attach.db < duckdb/test/sql/storage_version/generate_storage_version.sql
$DUCKDB_PATH  test/test_data/lineitem_sf1.db -c "CALL dbgen(sf=1)"
