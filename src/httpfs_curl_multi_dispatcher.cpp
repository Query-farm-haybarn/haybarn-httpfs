#include "httpfs_curl_multi_dispatcher.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace duckdb {

namespace {

// One PendingTransfer per blocked worker call to Submit(). The worker
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

// Signal a pending transfer. notify_one is called WHILE the lock is still
// held — the classic condvar-destruction race: if we drop the lock first,
// the worker can wake, observe done=true, return from Submit, and unwind
// its stack frame (destroying *pending) before our notify reaches `cv`.
// Holding the lock through notify keeps `pending` alive for the call.
static void SignalPending(PendingTransfer *pending, CURLcode result) {
	std::lock_guard<std::mutex> lk(pending->mu);
	pending->result = result;
	pending->done = true;
	pending->cv.notify_one();
}

class Dispatcher {
public:
	static Dispatcher &Instance() {
		// Function-local static — initialized on first call, destroyed at
		// static-destruction time. Thread-safe per C++11 §6.7.4.
		static Dispatcher inst;
		return inst;
	}

	CURLcode Submit(CURL *easy) {
		PendingTransfer pending;
		pending.easy = easy;
		{
			std::lock_guard<std::mutex> lk(queue_mu);
			// Reject if the dispatcher is shutting down (or already gone)
			// so we don't push into a doomed queue or wait on a cv that
			// nothing will ever signal. Worker's Execute path treats this
			// as a transient transfer failure; retry logic (if any) takes
			// over from there.
			if (rejecting) {
				return CURLE_ABORTED_BY_CALLBACK;
			}
			submit_queue.push_back(&pending);
		}
		// curl_multi_wakeup is thread-safe (since curl 7.68); all other
		// curl_multi_* calls live on the dispatcher thread.
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
		// If anything in this try block throws (most likely the std::thread
		// ctor under EAGAIN/pthread limits), tear the multi handle down
		// before rethrowing. Without this the function-local static would
		// leak a CURLM* on every retry attempt.
		try {
			// Allow HTTP/2 stream multiplexing on connections that
			// negotiate h2. Without this, even with PIPEWAIT on every
			// transfer, curl will not multiplex new transfers onto an
			// existing h2 connection.
			curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
			thread = std::thread([this] { Loop(); });
		} catch (...) {
			curl_multi_cleanup(multi);
			multi = nullptr;
			throw;
		}
	}

	~Dispatcher() {
		{
			// Set the rejecting flag and the stopping flag together under
			// queue_mu so any concurrent Submit either sees rejecting=true
			// and bails, OR has already pushed (and will be drained below).
			std::lock_guard<std::mutex> lk(queue_mu);
			rejecting = true;
			stopping.store(true);
		}
		curl_multi_wakeup(multi);
		if (thread.joinable()) {
			thread.join();
		}
		// Thread joined → no one else touches `multi`. Safe to clean up.
		curl_multi_cleanup(multi);
		multi = nullptr;
	}

	void Loop() {
		while (!stopping.load()) {
			try {
				LoopOnce();
			} catch (...) {
				// Defense in depth: the curl C APIs don't throw, and vector
				// allocation throwing after multi_add_handle still leaves the
				// transfer recoverable via multi_info_read on a later iteration
				// (the easy handle's CURLOPT_PRIVATE points to its
				// PendingTransfer regardless of whether we recorded it in
				// in_flight). Yield briefly so a persistent throw source
				// doesn't burn CPU spinning.
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		// Shutdown drain: signal BOTH queued-but-not-added transfers AND
		// in-flight transfers (already curl_multi_add_handle'd). The
		// original implementation only drained the queue, leaving in-flight
		// workers stuck on their condvars forever at process exit.
		FailAllPending(CURLE_ABORTED_BY_CALLBACK);
	}

	void LoopOnce() {
		// Drain the submission queue. Adding each handle to the multi here
		// (on the dispatcher thread) keeps curl_multi_* single-threaded.
		std::vector<PendingTransfer *> to_add;
		{
			std::lock_guard<std::mutex> lk(queue_mu);
			to_add.swap(submit_queue);
		}
		for (auto *pending : to_add) {
			curl_easy_setopt(pending->easy, CURLOPT_PRIVATE, pending);
			// PIPEWAIT tells curl: if there's an existing connection to
			// this host that supports multiplexing (i.e. h2), wait for it
			// and add a stream rather than opening a new TCP connection.
			// This is THE option that makes h2 multiplex actually happen
			// at the curl layer.
			curl_easy_setopt(pending->easy, CURLOPT_PIPEWAIT, 1L);
			curl_multi_add_handle(multi, pending->easy);
			in_flight.push_back(pending);
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
			// Remove BEFORE signaling — once the worker wakes it may
			// curl_easy_getinfo / re-Execute / destroy the handle, and
			// calling those on a handle still attached to the multi is UB.
			curl_multi_remove_handle(multi, msg->easy_handle);
			curl_easy_setopt(msg->easy_handle, CURLOPT_PRIVATE, nullptr);
			auto it = std::find(in_flight.begin(), in_flight.end(), pending);
			if (it != in_flight.end()) {
				in_flight.erase(it);
			}
			if (pending) {
				SignalPending(pending, result);
			}
		}

		// Wait for I/O on any active transfer or a curl_multi_wakeup from
		// a worker submitting new work. 100 ms is a fallback ceiling —
		// wakeup is the primary signal, this only fires when nothing
		// happens at all.
		curl_multi_poll(multi, nullptr, 0, 100, nullptr);
	}

	// Drain everything we know about: queued transfers and in-flight
	// transfers. Called once on shutdown.
	void FailAllPending(CURLcode result) {
		std::vector<PendingTransfer *> queued;
		{
			std::lock_guard<std::mutex> lk(queue_mu);
			queued.swap(submit_queue);
		}
		for (auto *pending : queued) {
			SignalPending(pending, result);
		}
		for (auto *pending : in_flight) {
			// Best-effort detach from the multi so the easy handle isn't
			// left attached when the worker resumes. multi is still valid
			// here — destructor calls curl_multi_cleanup only after the
			// thread has joined, which is AFTER this function returns.
			curl_multi_remove_handle(multi, pending->easy);
			curl_easy_setopt(pending->easy, CURLOPT_PRIVATE, nullptr);
			SignalPending(pending, result);
		}
		in_flight.clear();
	}

	CURLM *multi = nullptr;
	std::thread thread;
	std::atomic<bool> stopping {false};
	std::mutex queue_mu;
	// Both protected by queue_mu. `rejecting` is set in the destructor and
	// checked by Submit so late submitters return CURLE_ABORTED_BY_CALLBACK
	// instead of pushing into a doomed queue.
	bool rejecting = false;
	std::vector<PendingTransfer *> submit_queue;
	// Dispatcher-thread-only state (the dispatcher is the sole writer and
	// reader). Tracks transfers already passed to curl_multi_add_handle so
	// the shutdown drain can signal them — without this, transfers in the
	// multi at shutdown leave their workers stuck on a cv that nothing
	// will signal.
	std::vector<PendingTransfer *> in_flight;
};

} // namespace

CURLcode CurlMultiDispatcher::Execute(CURL *easy) {
	return Dispatcher::Instance().Submit(easy);
}

} // namespace duckdb
