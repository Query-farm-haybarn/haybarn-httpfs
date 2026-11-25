#!/usr/bin/env bash
# Regression check: no thread may outlive the unittest's exit-time teardown.
#
# The curl multi dispatcher (src/http/httpfs_curl_multi_dispatcher.cpp) used to
# detach its thread inside exit(). The thread's OpenSSL thread-exit destructor
# then ran concurrently with OPENSSL_cleanup and both freed the same state, so
# roughly one process exit in 10,000 on loaded CI runners ended in "double free
# or corruption" or a SIGSEGV after every test had passed.
#
# test/exit_teardown/exit_teardown_shim.c turns that rare race into a deterministic
# failure: it delays every non-main thread's TSD destructors and exits 97 if one
# is still running when exit() finishes. This script first proves the probe
# catches a thread that outlives exit, then runs an httpfs request (to a local
# HTTP server) under it.
#
# Usage: test/exit_teardown/run_exit_teardown_check.sh [iterations]   (BUILD=release|relassert|debug)
# Linux/glibc only; skips elsewhere.
set -euo pipefail

cd "$(dirname "$0")/../.."
iterations=${1:-3}
unittest="build/${BUILD:-release}/test/unittest"

if [[ "$(uname -s)" != Linux ]]; then
    echo "exit teardown check: Linux only, skipping"
    exit 0
fi
[[ -x "$unittest" ]] || { echo "exit teardown check: $unittest not built" >&2; exit 1; }

work=$(mktemp -d)
server_pid=
cleanup() {
    [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

cc=${CC:-cc}
"$cc" -O2 -Wall -shared -fPIC -o "$work/exit_teardown_shim.so" test/exit_teardown/exit_teardown_shim.c -ldl -lpthread

# 1. The probe must catch a detached thread whose destructors run after exit().
cat > "$work/outlives_exit.c" <<'C'
#include <pthread.h>
#include <unistd.h>
static pthread_key_t key;
static void dtor(void *p) { (void)p; }
static void *run(void *arg) {
    (void)arg;
    pthread_setspecific(key, &key);
    usleep(20 * 1000); /* still running when main returns */
    return 0;
}
int main(void) {
    pthread_t t;
    pthread_key_create(&key, dtor);
    pthread_create(&t, 0, run, 0);
    pthread_detach(t);
    usleep(1000);
    return 0;
}
C
"$cc" -O2 -o "$work/outlives_exit" "$work/outlives_exit.c" -lpthread
rc=0
LD_PRELOAD="$work/exit_teardown_shim.so" "$work/outlives_exit" 2> "$work/selftest.err" || rc=$?
if [[ "$rc" != 97 ]]; then
    echo "exit teardown check: the probe did not catch a thread outliving exit (status $rc)" >&2
    cat "$work/selftest.err" >&2
    exit 1
fi
echo "exit teardown check: probe self-test ok (caught a thread outliving exit)"

# 2. A local HTTP server for the httpfs request (an IP literal: no resolver threads).
printf 'exit teardown payload' > "$work/payload.txt"
python3 - "$work" > "$work/port" <<'PY' &
import functools, http.server, sys
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=sys.argv[1]))
print(srv.server_address[1], flush=True)
srv.serve_forever()
PY
server_pid=$!
for _ in $(seq 100); do [[ -s "$work/port" ]] && break; sleep 0.1; done
port=$(head -1 "$work/port")
[[ -n "$port" ]] || { echo "exit teardown check: HTTP server did not start" >&2; exit 1; }

failed=0
for i in $(seq 1 "$iterations"); do
    rc=0
    # An IP literal: a hostname lookup would add curl resolver threads, which
    # idle-exit on their own schedule and are not what this checks.
    HTTPFS_EXIT_TEARDOWN_URL="http://127.0.0.1:$port/payload.txt" \
        LD_PRELOAD="$work/exit_teardown_shim.so" \
        "$unittest" test/sql/exit_teardown/http_dispatcher_exit.test > "$work/out" 2> "$work/err" || rc=$?
    if [[ "$rc" != 0 ]] || ! grep -q "All tests passed" "$work/out" || ! grep -q "exit_teardown: ok" "$work/err"; then
        echo "exit teardown check: iteration $i failed (status $rc)" >&2
        cat "$work/out" "$work/err" >&2
        failed=1
    fi
done
if [[ "$failed" != 0 ]]; then
    exit 1
fi
echo "exit teardown check: $iterations/$iterations runs exited with no thread outliving teardown"
