#include "httpfs_client.hpp"
#include "http_state.hpp"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <atomic>

namespace duckdb {

class HTTPFSClient : public HTTPClient {
public:
	HTTPFSClient(HTTPFSParams &http_params, const string &proto_host_port) : HTTPClient(proto_host_port) {
		client = make_uniq<duckdb_httplib_openssl::Client>(proto_host_port);
		Initialize(http_params);
	}
	void Initialize(HTTPParams &http_p) override {
		HTTPFSParams &http_params = (HTTPFSParams &)http_p;
		client->set_follow_location(http_params.follow_location);
		client->set_keep_alive(http_params.keep_alive);
		if (!http_params.ca_cert_file.empty()) {
			client->set_ca_cert_path(http_params.ca_cert_file.c_str());
		} else {
			client->set_ca_cert_path("");
		}
		const bool verify_ssl =
		    http_params.override_verify_ssl ? http_params.verify_ssl : http_params.enable_server_cert_verification;
		client->enable_server_certificate_verification(verify_ssl);
		client->set_write_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_read_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_connection_timeout(http_params.timeout, http_params.timeout_usec);
		client->set_decompress(false);
		if (!http_params.bearer_token.empty()) {
			client->set_bearer_token_auth(http_params.bearer_token.c_str());
		} else {
			client->set_bearer_token_auth("");
		}

		if (!http_params.http_proxy.empty()) {
			client->set_proxy(http_params.http_proxy, http_params.http_proxy_port);

			if (!http_params.http_proxy_username.empty()) {
				client->set_proxy_basic_auth(http_params.http_proxy_username, http_params.http_proxy_password);
			}
		} else {
			client->set_proxy("", -1);
			client->set_proxy_basic_auth("", "");
		}
		state = http_params.state;
	}

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		if (state) {
			state->get_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		if (!info.response_handler && !info.content_handler) {
			return TransformResult(client->Get(info.path, headers));
		} else {
			return TransformResult(client->Get(
			    info.path.c_str(), headers,
			    [&](const duckdb_httplib_openssl::Response &response) {
				    auto http_response = TransformResponse(response);
				    return info.response_handler(*http_response);
			    },
			    [&](const char *data, size_t data_length) {
				    if (state) {
					    state->total_bytes_received += data_length;
				    }
				    return info.content_handler(const_data_ptr_cast(data), data_length);
			    }));
		}
	}
	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override {
		if (state) {
			state->put_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Put(info.path, headers, const_char_ptr_cast(info.buffer_in), info.buffer_in_len,
		                                   info.content_type));
	}

	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override {
		if (state) {
			state->head_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Head(info.path, headers));
	}

	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override {
		if (state) {
			state->delete_count++;
		}
		auto headers = TransformHeaders(info.headers, info.params);
		return TransformResult(client->Delete(info.path, headers));
	}

	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override {
		if (state) {
			state->post_count++;
			state->total_bytes_sent += info.buffer_in_len;
		}
		// We use a custom Request method here, because there is no Post call with a contentreceiver in httplib
		duckdb_httplib_openssl::Request req;
		if (info.send_post_as_get_request) {
			req.method = "GET";
		} else {
			req.method = "POST";
		}
		req.path = info.path;
		req.headers = TransformHeaders(info.headers, info.params);
		if (req.headers.find("Content-Type") == req.headers.end()) {
			req.headers.emplace("Content-Type", "application/octet-stream");
		}
		req.content_receiver = [&](const char *data, size_t data_length, uint64_t /*offset*/,
		                           uint64_t /*total_length*/) {
			// Abort once the caller's cancellation flag is observed set. This only fires while
			// response bytes are arriving (httplib has no upload-phase hook), so it is weaker
			// than the curl backend's progress callback — acceptable for this fallback client.
			if (info.cancellation && info.cancellation->load(std::memory_order_relaxed)) {
				return false;
			}
			if (state) {
				state->total_bytes_received += data_length;
			}
			info.buffer_out += string(data, data_length);
			return true;
		};
		// First assign body, this is the body that will be uploaded
		req.body.assign(const_char_ptr_cast(info.buffer_in), info.buffer_in_len);
		auto transformed_req = TransformResult(client->send(req));
		if (info.cancellation && info.cancellation->load(std::memory_order_relaxed)) {
			transformed_req->cancelled = true;
			transformed_req->request_error = "HTTP POST request was cancelled";
			return transformed_req;
		}
		// Then, after actual re-quest, re-assign body to the response value of the POST request
		transformed_req->body.assign(const_char_ptr_cast(info.buffer_in), info.buffer_in_len);
		return transformed_req;
	}

private:
	duckdb_httplib_openssl::Headers TransformHeaders(const HTTPHeaders &header_map, const HTTPParams &params) {
		auto &httpfs_params = params.Cast<HTTPFSParams>();

		auto is_accept_encoding = [](const std::string &name) {
			return StringUtil::Lower(name) == "accept-encoding";
		};

		duckdb_httplib_openssl::Headers headers;
		for (auto &entry : header_map) {
			if (is_accept_encoding(entry.first)) {
				continue;
			}
			headers.insert(entry);
		}
		if (!httpfs_params.pre_merged_headers) {
			for (auto &entry : params.extra_headers) {
				if (is_accept_encoding(entry.first)) {
					continue;
				}
				headers.insert(entry);
			}
		}
		// The bundled httplib does not reliably decode brotli/zstd, and we want a consistent
		// byte-exact invariant for the httplib path. Force identity on every request and
		// reject any non-identity Content-Encoding in TransformResponse. Users who want
		// response compression should switch to the curl client.
		headers.emplace("Accept-Encoding", "identity");
		return headers;
	}

	unique_ptr<HTTPResponse> TransformResponse(const duckdb_httplib_openssl::Response &response) {
		// We asked for Accept-Encoding: identity on every request. If the server applied a
		// transfer compression anyway, the body bytes are not the object bytes — fail loudly
		// rather than hand garbage to the reader.
		auto ce_it = response.headers.find("Content-Encoding");
		if (ce_it != response.headers.end() && StringUtil::Lower(ce_it->second) != "identity") {
			auto err = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			err->request_error = "Server returned Content-Encoding '" + ce_it->second +
			                     "' but the httplib client only supports identity encoding. Switch to the curl "
			                     "client (SET httpfs_client_implementation = 'curl') or configure the server to "
			                     "disable transfer compression.";
			for (auto &entry : response.headers) {
				err->headers.Insert(entry.first, entry.second);
			}
			return err;
		}

		auto status_code = HTTPUtil::ToStatusCode(response.status);
		auto result = make_uniq<HTTPResponse>(status_code);
		result->body = response.body;
		result->reason = response.reason;
		for (auto &entry : response.headers) {
			result->headers.Insert(entry.first, entry.second);
		}
		return result;
	}

	unique_ptr<HTTPResponse> TransformResult(duckdb_httplib_openssl::Result &&res) {
		if (res.error() == duckdb_httplib_openssl::Error::Success) {
			auto &response = res.value();
			return TransformResponse(response);
		} else {
			auto result = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			result->request_error = to_string(res.error());
			return result;
		}
	}

private:
	unique_ptr<duckdb_httplib_openssl::Client> client;
	optional_ptr<HTTPState> state;
};

unique_ptr<HTTPClient> HTTPFSUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	auto client = make_uniq<HTTPFSClient>(http_params.Cast<HTTPFSParams>(), proto_host_port);
	return std::move(client);
}

unordered_map<string, string> HTTPFSUtil::ParseGetParameters(const string &text) {
	duckdb_httplib_openssl::Params query_params;
	duckdb_httplib_openssl::detail::parse_query_text(text, query_params);

	unordered_map<string, string> result;
	for (auto &entry : query_params) {
		result.emplace(std::move(entry.first), std::move(entry.second));
	}
	return result;
}

} // namespace duckdb
