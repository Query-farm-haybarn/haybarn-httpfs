#pragma once

#include <curl/curl.h>

#include "duckdb/common/http_util.hpp"

namespace duckdb {
class HTTPLogger;
class FileOpener;
struct FileOpenerInfo;
class HTTPState;

class CURLHandle {
public:
	CURLHandle(const string &token, const string &cert_path);
	~CURLHandle();

public:
	operator CURL *() {
		return curl;
	}
	// When `use_multi_dispatch` is true the caller has opted into HTTP/2
	// stream multiplexing — Execute() dispatches via the process-global
	// curl_multi handle instead of curl_easy_perform. The multi path is
	// synchronous from the caller's POV (blocks until the transfer
	// completes) so call sites don't need to change.
	CURLcode Execute();

	void SetUseMultiDispatch(bool value) {
		use_multi_dispatch = value;
	}

private:
	CURL *curl = NULL;
	bool use_multi_dispatch = false;
};

class CURLRequestHeaders {
public:
	CURLRequestHeaders(vector<std::string> &input) {
		for (auto &header : input) {
			Add(header);
		}
	}
	CURLRequestHeaders() {
	}

	~CURLRequestHeaders() {
		if (headers) {
			curl_slist_free_all(headers);
		}
		headers = NULL;
	}
	operator bool() const {
		return headers != NULL;
	}

public:
	void Add(const string &header) {
		headers = curl_slist_append(headers, header.c_str());
	}

public:
	curl_slist *headers = NULL;
};

} // namespace duckdb
