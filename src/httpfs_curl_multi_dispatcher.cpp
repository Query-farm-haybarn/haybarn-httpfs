#include "httpfs_curl_multi_dispatcher.hpp"

#include "duckdb/common/exception.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace duckdb {

namespace {

// One PendingTransfer per blocked worker call to Execute(). The worker
// constructs it on its own stack, hands a pointer to the dispatcher via
// the submit queue, then waits on the embedded condvar. The dispatcher
// fills in `result`, sets `done`, and notifies — at which point the
// worker wakes up and the PendingTransfer is torn down with the call.
struct PendingTransfer {
	CURL *easy;
	CURLcode result = CURLE_FAILED_INIT;
	bool done = false;
	std::mutex mu;
	std::condition_variable cv;
};

class Dispatcher {
public:
	static Dispatcher &Instance() {
		// Function-local static — initialized on first call, destroyed
		// during process exit. Thread-safe per C++11 §6.7.4.
		static Dispatcher inst;
		return inst;
	}

	CURLcode Submit(CURL *easy) {
		PendingTransfer pending;
		pending.easy = easy;
		{
			std::lock_guard<std::mutex> lk(queue_mu);
			submit_queue.push_back(&pending);
		}
		// Break the dispatcher's poll so the new submission is picked up
		// immediately instead of waiting for the poll timeout to elapse.
		// curl_multi_wakeup is thread-safe (since curl 7.68); the rest of
		// curl_multi_* is not, which is why every other multi call lives
		// on the dispatcher thread.
		curl_multi_wakeup(multi);

		std::unique_lock<std::mutex> lk(pending.mu);
		pending.cv.wait(lk, [&] { return pending.done; });
		return pending.result;
	}

private:
	Dispatcher() {
		multi = curl_multi_init();
		if (!multi) {
			throw InternalException("curl_multi_init failed");
		}
		// Allow HTTP/2 stream multiplexing on connections that negotiate h2.
		// Without this, even with PIPEWAIT on every transfer, curl will not
		// multiplex new transfers onto an existing h2 connection.
		curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
		// We do NOT set CURLMOPT_MAX_HOST_CONNECTIONS=1: that would force
		// serialization on H/1.1 peers (no ALPN h2 → no multiplex → with
		// max=1 a per-host queue of 1). Leaving it at the default lets H/1.1
		// fall back to a normal connection pool while h2 peers multiplex.
		thread = std::thread([this] { Loop(); });
	}

	~Dispatcher() {
		stopping.store(true);
		curl_multi_wakeup(multi);
		if (thread.joinable()) {
			thread.join();
		}
		curl_multi_cleanup(multi);
	}

	void Loop() {
		while (!stopping.load()) {
			// Drain the submission queue. Adding each handle to the multi
			// here (on the dispatcher thread) keeps curl_multi_* single-
			// threaded.
			std::vector<PendingTransfer *> to_add;
			{
				std::lock_guard<std::mutex> lk(queue_mu);
				to_add.swap(submit_queue);
			}
			for (auto *pending : to_add) {
				curl_easy_setopt(pending->easy, CURLOPT_PRIVATE, pending);
				// PIPEWAIT tells curl: if there's an existing connection
				// to this host that supports multiplexing (i.e. h2), wait
				// for it and add a stream rather than opening a new TCP
				// connection. This is THE option that makes h2 multiplex
				// actually happen at the curl layer.
				curl_easy_setopt(pending->easy, CURLOPT_PIPEWAIT, 1L);
				curl_multi_add_handle(multi, pending->easy);
			}

			int running = 0;
			curl_multi_perform(multi, &running);

			// Harvest completions and wake the blocked workers.
			CURLMsg *msg;
			int msgs_left = 0;
			while ((msg = curl_multi_info_read(multi, &msgs_left)) != nullptr) {
				if (msg->msg != CURLMSG_DONE) {
					continue;
				}
				PendingTransfer *pending = nullptr;
				curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &pending);
				CURLcode result = msg->data.result;
				// Remove BEFORE signaling — once we signal, the worker
				// may call curl_easy_getinfo / re-Execute / destroy the
				// handle. Calling those on a handle still attached to the
				// multi would be undefined behavior.
				curl_multi_remove_handle(multi, msg->easy_handle);
				curl_easy_setopt(msg->easy_handle, CURLOPT_PRIVATE, nullptr);
				if (pending) {
					{
						std::lock_guard<std::mutex> lk(pending->mu);
						pending->result = result;
						pending->done = true;
					}
					pending->cv.notify_one();
				}
			}

			// Wait for I/O on any of the active transfers, or for a
			// curl_multi_wakeup from a worker submitting new work. 100 ms
			// is a fallback ceiling — wakeup is the primary signal, so
			// this only fires when there's no activity at all.
			curl_multi_poll(multi, nullptr, 0, 100, nullptr);
		}

		// Drain any in-flight transfers on shutdown so workers don't deadlock
		// waiting for a condvar that will never fire. Failure with CURLE_ABORTED_BY_CALLBACK
		// is the closest standard CURLcode to "we shut down on you."
		std::vector<PendingTransfer *> stragglers;
		{
			std::lock_guard<std::mutex> lk(queue_mu);
			stragglers.swap(submit_queue);
		}
		for (auto *pending : stragglers) {
			{
				std::lock_guard<std::mutex> lk(pending->mu);
				pending->result = CURLE_ABORTED_BY_CALLBACK;
				pending->done = true;
			}
			pending->cv.notify_one();
		}
	}

	CURLM *multi;
	std::thread thread;
	std::atomic<bool> stopping {false};
	std::mutex queue_mu;
	std::vector<PendingTransfer *> submit_queue;
};

} // namespace

CURLcode CurlMultiDispatcher::Execute(CURL *easy) {
	return Dispatcher::Instance().Submit(easy);
}

} // namespace duckdb
