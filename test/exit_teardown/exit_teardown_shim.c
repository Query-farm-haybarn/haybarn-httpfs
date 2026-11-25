// LD_PRELOAD probe for test/exit_teardown/run_exit_teardown_check.sh (Linux/glibc only).
//
// Detects a thread that is still exiting while the process runs its exit-time
// teardown. The curl multi dispatcher once detached its thread at exit;
// that thread's OpenSSL thread-exit destructor then ran concurrently with
// OPENSSL_cleanup and both freed the same state ("double free or corruption"
// or SIGSEGV after every test passed, about 1 exit in 10,000 on CI).
//
// The probe makes the race deterministic: it wraps every pthread TSD
// destructor, and on a non-main thread delays it by EXIT_TEARDOWN_DELAY_MS and
// counts it while it runs. Its own ELF destructor runs at the very end of
// exit() (_dl_fini, after every atexit handler and static destructor). If any
// thread-exit destructor is still running then, or starts during a short grace
// period, the process finished exiting while a thread was still tearing down,
// so the probe exits with status 97. A thread that is joined before exit
// returns cannot trip it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_KEYS 64
static void (*real_dtor[MAX_KEYS])(void *);
static atomic_int key_count;
static atomic_int in_flight;
static atomic_int started_after_fini;
static atomic_int fini_started;

static unsigned delay_ms(void) {
	const char *e = getenv("EXIT_TEARDOWN_DELAY_MS");
	return e ? (unsigned)atoi(e) : 50;
}

static void wrapped(int i, void *p) {
	if (syscall(SYS_gettid) == getpid()) {
		real_dtor[i](p);
		return;
	}
	if (atomic_load(&fini_started)) {
		atomic_fetch_add(&started_after_fini, 1);
	}
	atomic_fetch_add(&in_flight, 1);
	usleep(delay_ms() * 1000);
	real_dtor[i](p);
	atomic_fetch_sub(&in_flight, 1);
}

// One trampoline per wrapped key (pthread destructors carry no context).
#define W(i) static void w##i(void *p) { wrapped(i, p); }
W(0) W(1) W(2) W(3) W(4) W(5) W(6) W(7)
W(8) W(9) W(10) W(11) W(12) W(13) W(14) W(15)
W(16) W(17) W(18) W(19) W(20) W(21) W(22) W(23)
W(24) W(25) W(26) W(27) W(28) W(29) W(30) W(31)
W(32) W(33) W(34) W(35) W(36) W(37) W(38) W(39)
W(40) W(41) W(42) W(43) W(44) W(45) W(46) W(47)
W(48) W(49) W(50) W(51) W(52) W(53) W(54) W(55)
W(56) W(57) W(58) W(59) W(60) W(61) W(62) W(63)
static void (*const tramp[MAX_KEYS])(void *) = {
    w0, w1, w2, w3, w4, w5, w6, w7,
    w8, w9, w10, w11, w12, w13, w14, w15,
    w16, w17, w18, w19, w20, w21, w22, w23,
    w24, w25, w26, w27, w28, w29, w30, w31,
    w32, w33, w34, w35, w36, w37, w38, w39,
    w40, w41, w42, w43, w44, w45, w46, w47,
    w48, w49, w50, w51, w52, w53, w54, w55,
    w56, w57, w58, w59, w60, w61, w62, w63,
};

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *)) {
	static int (*real)(pthread_key_t *, void (*)(void *));
	if (!real) {
		real = (int (*)(pthread_key_t *, void (*)(void *)))dlsym(RTLD_NEXT, "pthread_key_create");
	}
	if (dtor) {
		int n = atomic_fetch_add(&key_count, 1);
		if (n < MAX_KEYS) {
			real_dtor[n] = dtor;
			return real(key, tramp[n]);
		}
	}
	return real(key, dtor);
}

__attribute__((destructor)) static void exit_teardown_check(void) {
	atomic_store(&fini_started, 1);
	// Give a detached thread that is about to start its destructors the chance to.
	usleep(2 * delay_ms() * 1000);
	int running = atomic_load(&in_flight);
	int late = atomic_load(&started_after_fini);
	if (running || late) {
		fprintf(stderr,
		        "exit_teardown: %d thread-exit destructor(s) still running and %d started after the process "
		        "finished its exit handlers: a thread outlived exit-time teardown\n",
		        running, late);
		_exit(97);
	}
	fprintf(stderr, "exit_teardown: ok (no thread outlived exit-time teardown)\n");
}
