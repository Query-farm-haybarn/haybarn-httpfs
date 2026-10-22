#include "catch.hpp"

#include "http/http_test_helper.hpp"
#include "http/httpfs_client.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <chrono>
#include <thread>

// Haybarn httpfs behaviour: Accept-Encoding policy, lowercase response header names, the If-Unmodified-Since
// precondition, and cancellation of in-flight requests.

namespace duckdb {

namespace {

const char *const LAST_MODIFIED = "Wed, 21 Oct 2026 07:28:00 GMT";

vector<string> HeaderValuesFor(const vector<MockS3RequestObservation> &observations, const string &method,
                               const string &name) {
	vector<string> result;
	for (auto &observation : observations) {
		if (observation.method != method) {
			continue;
		}
		auto values = MockS3HeaderValues(observation, name);
		result.insert(result.end(), values.begin(), values.end());
	}
	return result;
}

} // namespace

TEST_CASE("HTTP file reads ask for identity encoding", "[httpfs][haybarn][accept-encoding]") {
	for (const auto &client : {"curl", "httplib"}) {
		CAPTURE(client);
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 0, client);
		HTTPTestHelper::RequireQueryOk(con, "SET extra_http_headers = MAP {'Accept-Encoding': 'gzip'}");
		HTTPTestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
		auto data = HTTPTestHelper::ReadRange(con, server.HTTPPath(), 100, 64);
		HTTPTestHelper::RequireQueryOk(con, "COMMIT");
		REQUIRE(data == server.ObjectData().substr(100, 64));

		auto observations = server.Observations();
		INFO(MockS3DescribeObservations(observations));
		for (const auto &method : {"HEAD", "GET"}) {
			CAPTURE(method);
			// the caller-supplied gzip is dropped in favour of the client's policy
			REQUIRE(HeaderValuesFor(observations, method, "Accept-Encoding") == vector<string> {"identity"});
		}
	}
}

TEST_CASE("HTTP response header names are lowercase", "[httpfs][haybarn][headers]") {
	for (const auto &client : {"curl", "httplib"}) {
		CAPTURE(client);
		MockS3ServerConfig config;
		config.metadata.response_headers = {{"X-Haybarn-Mixed-Case", "value"}};
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 0, client);

		auto &http_util = HTTPUtil::Get(*db.instance);
		auto params = http_util.InitializeParameters(*db.instance, server.HTTPPath());
		HeadRequestInfo head(server.HTTPPath(), HTTPHeaders(), *params);
		auto response = http_util.Request(head);
		REQUIRE(response);
		REQUIRE(response->status == HTTPStatusCode::OK_200);
		bool found = false;
		for (auto &header : response->headers) {
			REQUIRE(header.first == StringUtil::Lower(header.first));
			found = found || header.first == "x-haybarn-mixed-case";
		}
		REQUIRE(found);
		// lookups stay case-insensitive
		REQUIRE(response->GetHeaderValue("X-Haybarn-Mixed-Case") == "value");
	}
}

TEST_CASE("HTTP reads without a strong ETag send If-Unmodified-Since", "[httpfs][haybarn][conditional-read]") {
	SECTION("weak ETag") {
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		config.metadata.etag = "W/\"haybarn-weak\"";
		config.metadata.response_headers = {{"Last-Modified", LAST_MODIFIED}};
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 0);
		HTTPTestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
		auto data = HTTPTestHelper::ReadRange(con, server.HTTPPath(), 0, 32);
		HTTPTestHelper::RequireQueryOk(con, "COMMIT");
		REQUIRE(data == server.ObjectData().substr(0, 32));

		auto observations = server.Observations();
		INFO(MockS3DescribeObservations(observations));
		REQUIRE(HeaderValuesFor(observations, "GET", "If-Unmodified-Since") == vector<string> {LAST_MODIFIED});
		REQUIRE(HeaderValuesFor(observations, "GET", "If-Match").empty());
	}
	SECTION("strong ETag keeps If-Match") {
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		config.metadata.response_headers = {{"Last-Modified", LAST_MODIFIED}};
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 0);
		HTTPTestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
		HTTPTestHelper::ReadRange(con, server.HTTPPath(), 0, 32);
		HTTPTestHelper::RequireQueryOk(con, "COMMIT");

		auto observations = server.Observations();
		INFO(MockS3DescribeObservations(observations));
		REQUIRE(HeaderValuesFor(observations, "GET", "If-Match").size() == 1);
		REQUIRE(HeaderValuesFor(observations, "GET", "If-Unmodified-Since").empty());
	}
	SECTION("unsafe_disable_etag_checks sends no precondition") {
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		config.metadata.etag = "W/\"haybarn-weak\"";
		config.metadata.response_headers = {{"Last-Modified", LAST_MODIFIED}};
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 0);
		HTTPTestHelper::RequireQueryOk(con, "SET unsafe_disable_etag_checks = true");
		HTTPTestHelper::RequireQueryOk(con, "BEGIN TRANSACTION");
		HTTPTestHelper::ReadRange(con, server.HTTPPath(), 0, 32);
		HTTPTestHelper::RequireQueryOk(con, "COMMIT");

		auto observations = server.Observations();
		REQUIRE(HeaderValuesFor(observations, "GET", "If-Unmodified-Since").empty());
	}
}

TEST_CASE("Interrupting a query cancels its in-flight HTTP GET", "[httpfs][haybarn][cancellation]") {
	// httplib only observes cancellation while body bytes arrive, so a stalled body is curl-only.
	for (const bool multiplex : {true, false}) {
		CAPTURE(multiplex);
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		config.full_get.block_until_released = true;
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 3);
		HTTPTestHelper::RequireQueryOk(con, "SET force_download = true");
		HTTPTestHelper::RequireQueryOk(con, string("SET http2_multiplex = ") + (multiplex ? "true" : "false"));
		HTTPTestHelper::RequireQueryOk(con, "SET http_timeout = 120");

		unique_ptr<QueryResult> result;
		const auto start = std::chrono::steady_clock::now();
		std::thread query(
		    [&]() { result = con.Query("SELECT octet_length(content) FROM read_blob('" + server.HTTPPath() + "')"); });
		const bool full_get_started = server.WaitForFullGet();
		con.Interrupt();
		query.join();
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
		server.ReleaseFullGet();

		REQUIRE(full_get_started);
		REQUIRE(result);
		INFO((result->HasError() ? result->GetError() : string("no error")));
		REQUIRE(result->HasError());
		// far below http_timeout; the stalled transfer was aborted, not timed out or retried
		REQUIRE(elapsed < 20);
		auto observations = server.Observations();
		INFO(MockS3DescribeObservations(observations));
		REQUIRE(HTTPTestHelper::CountRequests(observations, "GET", 200) <= 1);
	}
}

TEST_CASE("Interrupting a query ends its HTTP retry back-off", "[httpfs][haybarn][cancellation]") {
	// The engine's retry loop sleeps the back-off in slices and ends it once the opening client context is
	// interrupted, whichever client sends the requests.
	for (const auto &client : {"curl", "httplib"}) {
		CAPTURE(client);
		MockS3ServerConfig config;
		config.object.data = HTTPTestHelper::CreateObjectData(4096);
		// every GET is a retryable RequestTimeout
		config.failures.transient_get_failures = 100;
		MockS3Server server(std::move(config));

		DuckDB db(nullptr);
		Connection con(db);
		HTTPTestHelper::Configure(db, con, 10, client);
		HTTPTestHelper::RequireQueryOk(con, "SET force_download = true");
		// back-offs of up to 20 s, which an interrupted query used to sit out
		HTTPTestHelper::RequireQueryOk(con, "SET http_retry_wait_ms = 20000");

		unique_ptr<QueryResult> result;
		std::thread query(
		    [&]() { result = con.Query("SELECT octet_length(content) FROM read_blob('" + server.HTTPPath() + "')"); });
		auto failed_gets = [&]() {
			idx_t count = 0;
			for (auto &observation : server.Observations()) {
				count += observation.method == "GET" && observation.status == 400;
			}
			return count;
		};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (failed_gets() == 0 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		const auto interrupted = std::chrono::steady_clock::now();
		con.Interrupt();
		query.join();
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - interrupted)
		        .count();

		REQUIRE(failed_gets() >= 1);
		REQUIRE(result);
		INFO((result->HasError() ? result->GetError() : string("no error")));
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorType() == ExceptionType::INTERRUPT);
		REQUIRE(elapsed < 3000);
		// the interrupted back-off is never followed by another attempt
		REQUIRE(failed_gets() <= 2);
	}
}

TEST_CASE("HTTP version setting rejects unknown values", "[httpfs][haybarn][http2]") {
	MockS3ServerConfig config;
	MockS3Server server(std::move(config));

	DuckDB db(nullptr);
	Connection con(db);
	HTTPTestHelper::Configure(db, con, 0);
	for (const auto &version : {"auto", "1.1", "2.0"}) {
		CAPTURE(version);
		HTTPTestHelper::RequireQueryOk(con, string("SET http_version = '") + version + "'");
		auto result = con.Query("SELECT octet_length(content) FROM read_blob('" + server.HTTPPath() + "')");
		INFO((result->HasError() ? result->GetError() : string()));
		// the mock speaks HTTP/1.1 over plain http, which every preference accepts (2.0 falls back to h1 without TLS)
		REQUIRE_FALSE(result->HasError());
	}
	HTTPTestHelper::RequireQueryOk(con, "SET http_version = '3'");
	auto result = con.Query("SELECT octet_length(content) FROM read_blob('" + server.HTTPPath() + "')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Unknown http_version"));
}

} // namespace duckdb
