#pragma once

#include <curl/curl.h>

namespace duckdb {

// Process-global curl_multi dispatcher.
//
// Per libcurl docs, CURLOPT_PIPEWAIT — the option that makes new transfers
// wait for and multiplex onto an existing HTTP/2 connection — is a no-op
// inside curl_easy_perform. The only way to get real h2 stream multiplexing
// across worker threads is to drive the transfers through a curl_multi
// handle. curl_multi itself is not thread-safe (only one thread may call
// curl_multi_* APIs at a time), so we own the multi handle on a dedicated
// dispatcher thread; workers submit (easy, condvar) pairs and block.
//
// One dispatcher per process is enough: curl's per-host connection cache
// inside the multi handle already shares connections across all transfers
// it dispatches, regardless of which worker submitted them. Lazily spawned
// on first Execute(); shuts down at process exit.
class CurlMultiDispatcher {
public:
	// Submit an easy handle. Blocks the caller until the transfer completes
	// (or fails). Returns the CURLcode that curl_easy_perform would have.
	//
	// The easy handle must be fully configured (URL, headers, callbacks)
	// before submission. CURLOPT_PIPEWAIT is set internally on the handle
	// so curl will wait for and multiplex onto an existing h2 connection
	// when one exists. The caller retains ownership of the handle and may
	// safely call curl_easy_getinfo / re-Execute it after this returns.
	static CURLcode Execute(CURL *easy);
};

} // namespace duckdb
