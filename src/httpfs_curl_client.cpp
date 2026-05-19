#include "httpfs_client.hpp"
#include "http_state.hpp"
#include "duckdb/logging/logger.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <curl/curl.h>
#include <sys/stat.h>
#include "duckdb/common/exception/http_exception.hpp"

#ifndef EMSCRIPTEN
#include "httpfs_curl_client.hpp"
#include "httpfs_curl_multi_dispatcher.hpp"
#endif

namespace duckdb {

CURLcode CURLHandle::Execute() {
	if (use_multi_dispatch) {
		return CurlMultiDispatcher::Execute(curl);
	}
	return curl_easy_perform(curl);
}

// we statically compile in libcurl, which means the cert file location of the build machine is the
// place curl will look. But not every distro has this file in the same location, so we search a
// number of common locations and use the first one we find.
static std::string certFileLocations[] = {
    // Arch, Debian-based, Gentoo
    "/etc/ssl/certs/ca-certificates.crt",
    // RedHat 7 based
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    // Redhat 6 based
    "/etc/pki/tls/certs/ca-bundle.crt",
    // OpenSUSE
    "/etc/ssl/ca-bundle.pem",
    // Alpine
    "/etc/ssl/cert.pem"};

//! Grab the first path that exists, from a list of well-known locations
static std::string SelectCURLCertPath() {
	for (std::string &caFile : certFileLocations) {
		struct stat buf;
		if (stat(caFile.c_str(), &buf) == 0) {
			return caFile;
		}
	}
	return std::string();
}

static size_t RequestWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t totalSize = size * nmemb;
	std::string *str = static_cast<std::string *>(userp);
	str->append(static_cast<char *>(contents), totalSize);
	return totalSize;
}

static size_t RequestHeaderCallback(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t totalSize = size * nmemb;
	std::string header(static_cast<char *>(contents), totalSize);
	HeaderCollector *header_collection = static_cast<HeaderCollector *>(userp);

	// Trim trailing \r\n
	if (!header.empty() && header.back() == '\n') {
		header.pop_back();
		if (!header.empty() && header.back() == '\r') {
			header.pop_back();
		}
	}

	// If header starts with HTTP/... curl has followed a redirect and we have a new Header,
	// so we push back a new header_collection and store headers from the redirect there.
	if (header.rfind("HTTP/", 0) == 0) {
		header_collection->header_collection.push_back(HTTPHeaders());
		header_collection->header_collection.back().Insert("__RESPONSE_STATUS__", header);
	}

	size_t colonPos = header.find(':');

	if (colonPos != std::string::npos) {
		// Split the string into two parts
		std::string part1 = header.substr(0, colonPos);
		std::string part2 = header.substr(colonPos + 1);
		if (part2.at(0) == ' ') {
			part2.erase(0, 1);
		}

		header_collection->header_collection.back().Insert(part1, part2);
	}
	// TODO: log headers that don't follow the header format

	return totalSize;
}

CURLHandle::CURLHandle(const string &token, const string &cert_path) {
	curl = curl_easy_init();
	if (!curl) {
		throw InternalException("Failed to initialize curl");
	}
	if (!token.empty()) {
		curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, token.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
	}
	if (!cert_path.empty()) {
		curl_easy_setopt(curl, CURLOPT_CAINFO, cert_path.c_str());
	}
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_AUTO_CLIENT_CERT | CURLSSLOPT_NATIVE_CA);
}

CURLHandle::~CURLHandle() {
	curl_easy_cleanup(curl);
}

struct RequestInfo {
	string url = "";
	string body = "";
	uint16_t response_code = 0;
	std::vector<HTTPHeaders> header_collection;
	// True when we sent Accept-Encoding: identity for this request (i.e. it is a Range request
	// or otherwise requires byte-exact response semantics). Used to validate the response
	// Content-Encoding header before delivering the body to the caller.
	bool sent_identity_only = false;
};

static bool RequestIsRangeRequest(const HTTPHeaders &headers, const HTTPParams &params) {
	if (headers.HasHeader("Range")) {
		return true;
	}
	// extra_headers may carry the Range header when pre_merged_headers is false.
	for (auto &entry : params.extra_headers) {
		if (StringUtil::Lower(entry.first) == "range") {
			return true;
		}
	}
	return false;
}

static idx_t httpfs_client_count = 0;

class HTTPFSCurlClient : public HTTPClient {
public:
	static string NormalizePathToBeAdded(string added_path) {
		while (added_path.size() > 2) {
			if (StringUtil::StartsWith(added_path, "//"))
				added_path = added_path.substr(1);
			else if (StringUtil::StartsWith(added_path, "./"))
				added_path = added_path.substr(1);
			else
				break;
		}

		return added_path;
	}
	HTTPFSCurlClient(HTTPFSParams &http_params, const string &proto_host_port) : HTTPClient(proto_host_port) {
		curl_base_url = curl_url();
		string normalized_path = NormalizePathToBeAdded(proto_host_port);
		curl_url_set(curl_base_url, CURLUPART_URL, normalized_path.c_str(), 0);
		stored_bearer_token = "";
		stored_cert_file_path = "";
		Initialize(http_params);
	}
	void Initialize(HTTPParams &http_p) override {
		HTTPFSParams &http_params = (HTTPFSParams &)http_p;
		auto bearer_token = "";
		if (!http_params.bearer_token.empty()) {
			bearer_token = http_params.bearer_token.c_str();
		}

		state = http_params.state;

		std::string cert_file_path;
		if (!http_params.ca_cert_file.empty()) {
			cert_file_path = http_params.ca_cert_file;
		}

		if (!curl || (stored_bearer_token != bearer_token) || (stored_cert_file_path != cert_file_path)) {
			// call curl_global_init if not already done by another HTTPFS Client
			InitCurlGlobal();

			stored_cert_file_path = cert_file_path;

			if (cert_file_path.empty()) {
				cert_file_path = SelectCURLCertPath();
			}
			curl = make_uniq<CURLHandle>(bearer_token, cert_file_path);
			stored_bearer_token = bearer_token;
		}
		request_info = make_uniq<RequestInfo>();

		// set curl options

		// Curl re-uses connections by default
		if (!http_params.keep_alive) {
			curl_easy_setopt(*curl, CURLOPT_FORBID_REUSE, 1L);
		} else {
			curl_easy_setopt(*curl, CURLOPT_FORBID_REUSE, 0L);
		}

		const bool verify_ssl =
		    http_params.override_verify_ssl ? http_params.verify_ssl : http_params.enable_curl_server_cert_verification;
		if (verify_ssl) {
			curl_easy_setopt(*curl, CURLOPT_SSL_VERIFYPEER, 1L); // Verify the cert
			curl_easy_setopt(*curl, CURLOPT_SSL_VERIFYHOST, 2L); // Verify that the cert matches the hostname
		} else {
			curl_easy_setopt(*curl, CURLOPT_SSL_VERIFYPEER, 0L); // Override default, don't verify the cert
			curl_easy_setopt(*curl, CURLOPT_SSL_VERIFYHOST,
			                 0L); // Override default, don't verify that the cert matches the hostname
		}

		// set connection timeout
		curl_easy_setopt(*curl, CURLOPT_CONNECTTIMEOUT, http_params.timeout);
		// Do NOT impose a hard timeout on the whole transfer
		curl_easy_setopt(*curl, CURLOPT_TIMEOUT, 0L);                         // no hard timeout for uploads
		curl_easy_setopt(*curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);              // abort if < 1 KB/s...
		curl_easy_setopt(*curl, CURLOPT_LOW_SPEED_TIME, http_params.timeout); // ...for 'timeout' consecutive seconds

		// HTTP version preference. "auto" attempts HTTP/2 over TLS via ALPN and
		// transparently falls back to HTTP/1.1 if the peer doesn't negotiate h2 —
		// this is the right default in 2026: AWS S3, GitHub, GCS, HuggingFace,
		// and Cloudflare all support h2; servers that don't will fall back
		// automatically. Plain http:// stays on 1.1 (CURL_HTTP_VERSION_2TLS does
		// not attempt h2c). Users on servers that misbehave under h2 can opt out
		// via `SET http_version='1.1'`.
		long curl_http_version_opt = CURL_HTTP_VERSION_2TLS;
		if (http_params.http_version == "1.1") {
			curl_http_version_opt = CURL_HTTP_VERSION_1_1;
		} else if (http_params.http_version == "2.0" || http_params.http_version == "2") {
			curl_http_version_opt = CURL_HTTP_VERSION_2_0;
		} else if (http_params.http_version != "auto" && !http_params.http_version.empty()) {
			throw InvalidInputException("Unknown http_version '%s' (expected 'auto', '1.1', or '2.0')",
			                            http_params.http_version);
		}
		curl_easy_setopt(*curl, CURLOPT_HTTP_VERSION, curl_http_version_opt);
		// Surface protocol-level details (TLS handshake, ALPN selection, HTTP/2 frames)
		// when the user wants to verify negotiation. Off by default — curl's verbose
		// output is noisy and not appropriate for production.
		curl_easy_setopt(*curl, CURLOPT_VERBOSE, http_params.curl_verbose ? 1L : 0L);
		// Route through the global curl_multi dispatcher when h2 multiplexing is
		// requested. The dispatcher itself sets CURLOPT_PIPEWAIT on the handle so
		// transfers wait for and multiplex onto an existing h2 connection.
		curl->SetUseMultiDispatch(http_params.http2_multiplex);
		// Accept-Encoding is set per-request in the request methods below — Range requests
		// get "identity" (and the response is validated to enforce byte-exact semantics),
		// non-Range requests get "" so curl negotiates and transparently decodes every
		// encoding it was built with (gzip, deflate, brotli, zstd).
		// follow redirects
		curl_easy_setopt(*curl, CURLOPT_FOLLOWLOCATION, http_params.follow_location ? 1L : 0L);

		// define the header callback
		curl_easy_setopt(*curl, CURLOPT_HEADERFUNCTION, RequestHeaderCallback);
		curl_easy_setopt(*curl, CURLOPT_HEADERDATA, &request_info->header_collection);
		// define the write data callback (for get requests)
		curl_easy_setopt(*curl, CURLOPT_WRITEFUNCTION, RequestWriteCallback);
		curl_easy_setopt(*curl, CURLOPT_WRITEDATA, &request_info->body);

		// Reset PROXY-related settings, so they are set only on proxy actually being there
		curl_easy_setopt(*curl, CURLOPT_PROXY, NULL);
		curl_easy_setopt(*curl, CURLOPT_PROXYUSERNAME, NULL);
		curl_easy_setopt(*curl, CURLOPT_PROXYPASSWORD, NULL);

		if (!http_params.http_proxy.empty()) {
			curl_easy_setopt(*curl, CURLOPT_PROXY,
			                 StringUtil::Format("%s:%d", http_params.http_proxy, http_params.http_proxy_port).c_str());

			if (!http_params.http_proxy_username.empty()) {
				curl_easy_setopt(*curl, CURLOPT_PROXYUSERNAME, http_params.http_proxy_username.c_str());
				curl_easy_setopt(*curl, CURLOPT_PROXYPASSWORD, http_params.http_proxy_password.c_str());
			}
		}
	}

	~HTTPFSCurlClient() {
		curl_url_cleanup(curl_base_url);
		DestroyCurlGlobal();
	}
	static void AddUserAgentIfAvailable(HTTPFSParams &http_params, HTTPHeaders &header_map) {
		if (!http_params.user_agent.empty()) {
			header_map.Insert("User-Agent", http_params.user_agent);
		}
	}

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		AddUserAgentIfAvailable(static_cast<HTTPFSParams &>(info.params), info.headers);
		ResetRequestInfo();
		if (state) {
			state->get_count++;
		}

		auto curl_headers = TransformHeadersCurl(info.headers, info.params);
		AllowCompressedResponse(RequestIsRangeRequest(info.headers, info.params));
		request_info->url = info.url;

		CURLcode res;
		{
			curl_easy_setopt(*curl, CURLOPT_NOBODY, 0L);
			curl_easy_setopt(*curl, CURLOPT_HTTPGET, 1L);
			CURLU *url = curl_url_dup(curl_base_url);

			string normalized_path = NormalizePathToBeAdded(info.path);
			curl_url_set(url, CURLUPART_URL, normalized_path.c_str(), 0);

			curl_easy_setopt(*curl, CURLOPT_URL, nullptr);
			curl_easy_setopt(*curl, CURLOPT_CURLU, url);
			curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, curl_headers ? curl_headers.headers : nullptr);

			res = curl->Execute();
			curl_url_cleanup(url);
		}

		curl_easy_getinfo(*curl, CURLINFO_RESPONSE_CODE, &request_info->response_code);

		if (auto reject = RejectIfContentEncodingViolation()) {
			return reject;
		}

		const idx_t bytes_received = request_info->body.size();
		// Only sanity-check Content-Length against received bytes when the response is not
		// Content-Encoded. When curl auto-decodes a non-identity response, bytes_received is the
		// decoded size while Content-Length is the wire size — the comparison would be apples to
		// oranges. Range responses (which set sent_identity_only) are already validated above to
		// have identity encoding by the time we reach here.
		const bool response_is_identity =
		    request_info->header_collection.empty() ||
		    !request_info->header_collection.back().HasHeader("Content-Encoding") ||
		    StringUtil::Lower(request_info->header_collection.back().GetHeaderValue("Content-Encoding")) == "identity";
		if (response_is_identity && !request_info->header_collection.empty() &&
		    request_info->header_collection.back().HasHeader("content-length")) {
			try {
				// Use stoull (not stoi) — Content-Length can exceed INT_MAX for files >2GB.
				const idx_t content_length_received =
				    std::stoull(request_info->header_collection.back().GetHeaderValue("content-length"));
				if (bytes_received != content_length_received) {
					// Something is off, might happen in case of unreliable network
					// TODO: consider logging this
				}
			} catch (const std::exception &) {
				// Content-Length header contains a non-numeric value — skip validation.
			}
		}

		if (state) {
			state->total_bytes_received += bytes_received;
		}

		if (info.response_handler) {
			auto response = TransformResponseCurl(res);
			if (!info.response_handler(*response)) {
				return response;
			}
		}

		const char *data = request_info->body.c_str();
		if (info.content_handler && res == CURLcode::CURLE_OK) {
			info.content_handler(const_data_ptr_cast(data), bytes_received);
		}

		return TransformResponseCurl(res);
	}

	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override {
		AddUserAgentIfAvailable(static_cast<HTTPFSParams &>(info.params), info.headers);
		ResetRequestInfo();
		if (state) {
			state->put_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}

		auto curl_headers = TransformHeadersCurl(info.headers, info.params);
		AllowCompressedResponse(RequestIsRangeRequest(info.headers, info.params));
		// Add content type header from info
		curl_headers.Add("Content-Type: " + info.content_type);
		// transform parameters
		request_info->url = info.url;

		CURLcode res;
		{
			CURLU *url = curl_url_dup(curl_base_url);

			string normalized_path = NormalizePathToBeAdded(info.path);
			curl_url_set(url, CURLUPART_URL, normalized_path.c_str(), 0);

			curl_easy_setopt(*curl, CURLOPT_URL, nullptr);
			curl_easy_setopt(*curl, CURLOPT_CURLU, url);

			// Perform PUT
			curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, "PUT");
			// Include PUT body
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDS, const_char_ptr_cast(info.buffer_in));
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDSIZE, info.buffer_in_len);

			// Apply headers
			curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, curl_headers ? curl_headers.headers : nullptr);

			res = curl->Execute();
			curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, nullptr);
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDS, nullptr);
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDSIZE, 0);
			curl_url_cleanup(url);
		}

		curl_easy_getinfo(*curl, CURLINFO_RESPONSE_CODE, &request_info->response_code);

		if (auto reject = RejectIfContentEncodingViolation()) {
			return reject;
		}

		return TransformResponseCurl(res);
	}

	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override {
		AddUserAgentIfAvailable(static_cast<HTTPFSParams &>(info.params), info.headers);
		ResetRequestInfo();
		if (state) {
			state->head_count++;
		}

		auto curl_headers = TransformHeadersCurl(info.headers, info.params);
		RequireIdentityResponse();
		request_info->url = info.url;
		// transform parameters

		CURLcode res;
		{
			// Perform HEAD request instead of GET
			curl_easy_setopt(*curl, CURLOPT_NOBODY, 1L);
			curl_easy_setopt(*curl, CURLOPT_HTTPGET, 0L);

			CURLU *url = curl_url_dup(curl_base_url);

			string normalized_path = NormalizePathToBeAdded(info.path);
			curl_url_set(url, CURLUPART_URL, normalized_path.c_str(), 0);

			curl_easy_setopt(*curl, CURLOPT_URL, nullptr);
			curl_easy_setopt(*curl, CURLOPT_CURLU, url);

			// Add headers if any
			curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, curl_headers ? curl_headers.headers : nullptr);

			// Execute HEAD request
			res = curl->Execute();
			curl_easy_setopt(*curl, CURLOPT_NOBODY, 0L);
			curl_easy_setopt(*curl, CURLOPT_HTTPGET, 1L);
			curl_url_cleanup(url);
		}

		curl_easy_getinfo(*curl, CURLINFO_RESPONSE_CODE, &request_info->response_code);
		if (auto reject = RejectIfContentEncodingViolation()) {
			return reject;
		}
		return TransformResponseCurl(res);
	}

	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override {
		AddUserAgentIfAvailable(static_cast<HTTPFSParams &>(info.params), info.headers);
		ResetRequestInfo();
		if (state) {
			state->delete_count++;
		}
		auto curl_headers = TransformHeadersCurl(info.headers, info.params);
		AllowCompressedResponse(RequestIsRangeRequest(info.headers, info.params));
		// transform parameters
		request_info->url = info.url;

		CURLcode res;
		{
			CURLU *url = curl_url_dup(curl_base_url);

			string normalized_path = NormalizePathToBeAdded(info.path);
			curl_url_set(url, CURLUPART_URL, normalized_path.c_str(), 0);

			curl_easy_setopt(*curl, CURLOPT_URL, nullptr);
			curl_easy_setopt(*curl, CURLOPT_CURLU, url);

			// Set DELETE request method
			curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, "DELETE");

			// Add headers if any
			curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, curl_headers ? curl_headers.headers : nullptr);

			// Execute DELETE request
			res = curl->Execute();
			curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, nullptr);
			curl_url_cleanup(url);
		}

		// Get HTTP response status code
		curl_easy_getinfo(*curl, CURLINFO_RESPONSE_CODE, &request_info->response_code);
		if (auto reject = RejectIfContentEncodingViolation()) {
			return reject;
		}
		return TransformResponseCurl(res);
	}

	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override {
		AddUserAgentIfAvailable(static_cast<HTTPFSParams &>(info.params), info.headers);
		ResetRequestInfo();
		if (state) {
			state->post_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}

		auto curl_headers = TransformHeadersCurl(info.headers, info.params);
		AllowCompressedResponse(RequestIsRangeRequest(info.headers, info.params));
		if (!info.headers.HasHeader("Content-Type")) {
			const string content_type = "Content-Type: application/octet-stream";
			curl_headers.Add(content_type.c_str());
		}
		// transform parameters
		request_info->url = info.url;

		CURLcode res;
		{
			CURLU *url = curl_url_dup(curl_base_url);

			string normalized_path = NormalizePathToBeAdded(info.path);
			curl_url_set(url, CURLUPART_URL, normalized_path.c_str(), 0);

			curl_easy_setopt(*curl, CURLOPT_URL, nullptr);
			curl_easy_setopt(*curl, CURLOPT_CURLU, url);
			if (info.send_post_as_get_request) {
				curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, "GET");
			} else {
				curl_easy_setopt(*curl, CURLOPT_POST, 1L);
			}
			// Set POST body
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDS, const_char_ptr_cast(info.buffer_in));
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDSIZE, info.buffer_in_len);

			// Add headers if any
			curl_easy_setopt(*curl, CURLOPT_HTTPHEADER, curl_headers ? curl_headers.headers : nullptr);

			// Execute POST request
			res = curl->Execute();
			curl_easy_setopt(*curl, CURLOPT_CUSTOMREQUEST, nullptr);
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDS, nullptr);
			curl_easy_setopt(*curl, CURLOPT_POSTFIELDSIZE, 0);
			curl_easy_setopt(*curl, CURLOPT_POST, 0L);
			curl_url_cleanup(url);
		}

		curl_easy_getinfo(*curl, CURLINFO_RESPONSE_CODE, &request_info->response_code);

		if (auto reject = RejectIfContentEncodingViolation()) {
			return reject;
		}

		info.buffer_out = request_info->body;

		const idx_t bytes_received = request_info->body.size();
		if (state) {
			state->total_bytes_received += bytes_received;
		}

		// Construct HTTPResponse
		return TransformResponseCurl(res);
	}

	void Cleanup() override {
		// Release any buffers retained from the last request before this client is parked in the connection cache.
		request_info = make_uniq<RequestInfo>();
	}

private:
	CURLRequestHeaders TransformHeadersCurl(const HTTPHeaders &header_map, const HTTPParams &params) {
		auto &httpfs_params = params.Cast<HTTPFSParams>();

		// Drop any caller-supplied Accept-Encoding; the curl client owns Accept-Encoding policy
		// per-request (identity for Range, "" for non-Range). Leaving a caller-set value in
		// place is a footgun: with our previous unconditional CURLOPT_ACCEPT_ENCODING=NULL it
		// produced compressed responses we never decoded.
		auto is_accept_encoding = [](const std::string &name) {
			return StringUtil::Lower(name) == "accept-encoding";
		};

		std::vector<std::string> headers;
		for (auto &entry : header_map) {
			if (is_accept_encoding(entry.first)) {
				continue;
			}
			const std::string new_header = entry.first + ": " + entry.second;
			headers.push_back(new_header);
		}
		CURLRequestHeaders curl_headers;
		for (auto &header : headers) {
			curl_headers.Add(header);
		}
		if (!httpfs_params.pre_merged_headers) {
			for (auto &entry : params.extra_headers) {
				if (is_accept_encoding(entry.first)) {
					continue;
				}
				curl_headers.Add(entry.first + ": " + entry.second);
			}
		}
		return std::move(curl_headers);
	}

	void ResetRequestInfo() {
		// clear headers after transform
		request_info->header_collection.clear();
		// reset request info.
		request_info->body = "";
		request_info->url = "";
		request_info->response_code = 0;
		request_info->sent_identity_only = false;
	}

	// Send Accept-Encoding: identity and remember that the response must be identity-encoded.
	// Used for GET / HEAD / PUT / DELETE — these all participate in a file-read lifecycle
	// (HEAD probe followed by Range GETs, or PUT/DELETE on file objects), and we need
	// consistent server responses across that lifecycle. Servers like GitHub return
	// different ETags for gzip vs identity (`W/"..."` weak vs `"..."` strong); mixing
	// encodings within a file's lifetime trips our ETag-mismatch detection in httpfs.cpp.
	void RequireIdentityResponse() {
		curl_easy_setopt(*curl, CURLOPT_ACCEPT_ENCODING, "identity");
		request_info->sent_identity_only = true;
	}

	// Used for POST only: API calls (S3 LIST, multipart complete, HuggingFace API, …) live
	// outside the file-read lifecycle and benefit meaningfully from wire compression on the
	// response (highly compressible XML/JSON). Empty string asks curl to advertise every
	// encoding it was built with and decode transparently. The has_range_header guard is
	// defensive — a POST with a Range header is degenerate but if it ever happens we still
	// want byte-exact response semantics.
	void AllowCompressedResponse(bool has_range_header) {
		if (has_range_header) {
			RequireIdentityResponse();
			return;
		}
		curl_easy_setopt(*curl, CURLOPT_ACCEPT_ENCODING, "");
		request_info->sent_identity_only = false;
	}

	// If we asked for identity and the server returned a non-identity Content-Encoding, the
	// body bytes are not the object bytes — reject the response before any caller content
	// handler runs. Returns a populated error HTTPResponse, or nullptr if the response is OK
	// to deliver.
	unique_ptr<HTTPResponse> RejectIfContentEncodingViolation() {
		if (!request_info->sent_identity_only) {
			return nullptr;
		}
		if (request_info->header_collection.empty()) {
			return nullptr;
		}
		auto &headers = request_info->header_collection.back();
		if (!headers.HasHeader("Content-Encoding")) {
			return nullptr;
		}
		auto value = headers.GetHeaderValue("Content-Encoding");
		if (StringUtil::Lower(value) == "identity") {
			return nullptr;
		}
		auto response = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
		response->request_error = "Server returned Content-Encoding '" + value +
		                          "' but we requested identity; refusing to deliver non-identity-encoded bytes.";
		response->url = request_info->url;
		return response;
	}

	unique_ptr<HTTPResponse> TransformResponseCurl(CURLcode res) {
		auto status_code = HTTPStatusCode(request_info->response_code);
		auto response = make_uniq<HTTPResponse>(status_code);
		if (res != CURLcode::CURLE_OK) {
			response->request_error = curl_easy_strerror(res);
			return response;
		}
		response->body = request_info->body;
		response->url = request_info->url;
		response->reason = HTTPUtil::GetStatusMessage(HTTPUtil::ToStatusCode(request_info->response_code));
		if (!request_info->header_collection.empty()) {
			for (auto &header : request_info->header_collection.back()) {
				// We should not return __RESPONSE_STATUS__ to the user. It's only there for debugging.
				if (header.first == "__RESPONSE_STATUS__") {
					continue;
				}
				response->headers.Insert(header.first, header.second);
			}
		}
		// ResetRequestInfo();
		return response;
	}

private:
	unique_ptr<CURLHandle> curl;
	optional_ptr<HTTPState> state;
	unique_ptr<RequestInfo> request_info;
	CURLU *curl_base_url = nullptr;
	string stored_bearer_token;
	string stored_cert_file_path;

	static mutex &GetRefLock() {
		static mutex mtx;
		return mtx;
	}

	static void InitCurlGlobal() {
		const std::lock_guard<std::mutex> lock(GetRefLock());
		if (httpfs_client_count == 0) {
			curl_global_init(CURL_GLOBAL_DEFAULT);
		}
		++httpfs_client_count;
	}

	static void DestroyCurlGlobal() {
		// TODO: when to call curl_global_cleanup()
		// calling it on client destruction causes SSL errors when verification is on (due to many requests).
		// GetRefLock();
		// if (httpfs_client_count == 0) {
		// 	throw InternalException("Destroying Httpfs client that did not initialize CURL");
		// }
		// --httpfs_client_count;
		// if (httpfs_client_count == 0) {
		// 	curl_global_cleanup();
		// }
	}
};

unique_ptr<HTTPClient> HTTPFSCurlUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	if (connection_caching_enabled) {
		auto client = connection_cache.Find(proto_host_port);
		if (client) {
			if (http_params.logger &&
			    http_params.logger->ShouldLog(HTTPFSInfoLogType::NAME, HTTPFSInfoLogType::LEVEL)) {
				http_params.logger->WriteLog(
				    HTTPFSInfoLogType::NAME, HTTPFSInfoLogType::LEVEL,
				    HTTPFSInfoLogType::ConstructLogMessage("connection_cache_hit", proto_host_port));
			}
			client->Initialize(http_params);
			return client;
		}
		if (http_params.logger && http_params.logger->ShouldLog(HTTPFSInfoLogType::NAME, HTTPFSInfoLogType::LEVEL)) {
			http_params.logger->WriteLog(
			    HTTPFSInfoLogType::NAME, HTTPFSInfoLogType::LEVEL,
			    HTTPFSInfoLogType::ConstructLogMessage("connection_cache_miss", proto_host_port));
		}
	}
	auto client = make_uniq<HTTPFSCurlClient>(http_params.Cast<HTTPFSParams>(), proto_host_port);
	return std::move(client);
}

unordered_map<string, string> HTTPFSCurlUtil::ParseGetParameters(const string &text) {
	unordered_map<std::string, std::string> params;

	auto pos = text.find('?');
	if (pos == std::string::npos)
		return params;

	std::string query = text.substr(pos + 1);
	std::stringstream ss(query);
	std::string item;

	while (std::getline(ss, item, '&')) {
		auto eq_pos = item.find('=');
		if (eq_pos != std::string::npos) {
			std::string key = item.substr(0, eq_pos);
			std::string value = StringUtil::URLDecode(item.substr(eq_pos + 1));
			params[key] = value;
		} else {
			params[item] = ""; // key with no value
		}
	}

	return params;
}

string HTTPFSCurlUtil::GetName() const {
	return "HTTPFS-Curl";
}

} // namespace duckdb
