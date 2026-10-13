#define _POSIX_C_SOURCE 200809L
#include "download_stream.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHUNK (256 * 1024)
#define READ_ERROR (-5)

struct HostThread { pthread_t id; void (*entry)(void *); void *arg; };
static bool fail_thread_create;

static u8 pattern(u64 offset) {
	return (u8)((offset * 17) ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24));
}

void LightLock_Init(LightLock *lock) { assert(!pthread_mutex_init(lock, NULL)); }
void LightLock_Lock(LightLock *lock) { assert(!pthread_mutex_lock(lock)); }
void LightLock_Unlock(LightLock *lock) { assert(!pthread_mutex_unlock(lock)); }
void CondVar_Init(CondVar *cv) { assert(!pthread_cond_init(cv, NULL)); }
void CondVar_Wait(CondVar *cv, LightLock *lock) { assert(!pthread_cond_wait(cv, lock)); }
void CondVar_Broadcast(CondVar *cv) { assert(!pthread_cond_broadcast(cv)); }
int CondVar_WaitTimeout(CondVar *cv, LightLock *lock, s64 timeout) {
	struct timespec until;
	assert(!clock_gettime(CLOCK_REALTIME, &until));
	until.tv_sec += timeout / 1000000000;
	until.tv_nsec += timeout % 1000000000;
	if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
	return pthread_cond_timedwait(cv, lock, &until);
}
static void *host_entry(void *arg) {
	Thread thread = arg;
	thread->entry(thread->arg);
	return NULL;
}
Thread threadCreate(void (*entry)(void *), void *arg, size_t stack, int priority, int core, bool detached) {
	(void)stack; (void)priority; (void)core; (void)detached;
	if (fail_thread_create) return NULL;
	Thread thread = malloc(sizeof *thread);
	assert(thread);
	thread->entry = entry;
	thread->arg = arg;
	assert(!pthread_create(&thread->id, NULL, host_entry, thread));
	return thread;
}
Result threadJoin(Thread thread, u64 timeout) {
	(void)timeout;
	assert(!pthread_join(thread->id, NULL));
	return 0;
}
void threadFree(Thread thread) { free(thread); }

Result httpcDownloadData(httpcContext *context, u8 *buffer, u32 size, u32 *got) {
	LightLock_Lock(&context->lock);
	context->calls++;
	CondVar_Broadcast(&context->changed);
	while (context->blocked && !context->cancelled)
		CondVar_Wait(&context->changed, &context->lock);
	if (context->cancelled) { *got = 0; LightLock_Unlock(&context->lock); return READ_ERROR; }
	u64 end = context->length < context->fail_at ? context->length : context->fail_at;
	u64 left = context->offset < end ? end - context->offset : 0;
	*got = left < size ? left : size;
	for (u32 i = 0; i < *got; i++) buffer[i] = pattern(context->offset + i);
	context->offset += *got;
	Result result = context->offset >= context->fail_at ? READ_ERROR :
		(context->offset == context->length ? 0 : (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
	LightLock_Unlock(&context->lock);
	return result;
}
Result httpcCancelConnection(httpcContext *context) {
	LightLock_Lock(&context->lock);
	context->cancelled = true;
	CondVar_Broadcast(&context->changed);
	LightLock_Unlock(&context->lock);
	return 0;
}

static void init(httpcContext *context, u64 length) {
	*context = (httpcContext){ .length = length, .fail_at = U64_MAX };
	LightLock_Init(&context->lock);
	CondVar_Init(&context->changed);
}
static void cleanup(httpcContext *context) {
	assert(!pthread_cond_destroy(&context->changed));
	assert(!pthread_mutex_destroy(&context->lock));
}
static void wait_calls(httpcContext *context, unsigned count) {
	LightLock_Lock(&context->lock);
	while (context->calls < count)
		assert(!CondVar_WaitTimeout(&context->changed, &context->lock, 1000000000));
	LightLock_Unlock(&context->lock);
}
static void check_transfer(u64 length, u64 fail_at) {
	httpcContext context;
	init(&context, length);
	context.fail_at = fail_at;
	DownloadStream *stream = download_stream_start(&context);
	assert(stream);
	u64 written = 0;
	Result result = 0;
	for (;;) {
		const u8 *data;
		u32 size;
		int state = download_stream_next(stream, &data, &size, &result);
		if (state == DOWNLOAD_FINISHED) break;
		if (state == DOWNLOAD_WAITING) continue;
		assert(size && size <= CHUNK);
		for (u32 i = 0; i < size; i++) assert(data[i] == pattern(written + i));
		written += size;
		download_stream_release(stream);
	}
	assert(written == (length < fail_at ? length : fail_at));
	assert(result == (fail_at <= length ? READ_ERROR : 0));
	download_stream_stop(stream);
	cleanup(&context);
}

int main(void) {
	// Empty, partial, aligned, and many wraps of the ring buffer.
	check_transfer(0, U64_MAX);
	check_transfer(17, U64_MAX);
	check_transfer(CHUNK, U64_MAX);
	check_transfer(37 * CHUNK + 17, U64_MAX);
	// An HTTP error may arrive with a partial final chunk.
	check_transfer(10 * CHUNK, CHUNK + 17);

	httpcContext context;
	init(&context, 20 * CHUNK);
	DownloadStream *stream = download_stream_start(&context);
	assert(stream);
	const u8 *data;
	u32 size;
	Result result;
	while (download_stream_next(stream, &data, &size, &result) == DOWNLOAD_WAITING) {}
	wait_calls(&context, 4);
	// A held chunk must remain intact while the producer fills the queue.
	for (u32 i = 0; i < size; i++) assert(data[i] == pattern(i));
	LightLock_Lock(&context.lock);
	assert(context.calls == 4);
	LightLock_Unlock(&context.lock);
	download_stream_stop(stream); // producer blocked by the full queue
	cleanup(&context);

	init(&context, CHUNK);
	context.blocked = true;
	stream = download_stream_start(&context);
	assert(stream);
	wait_calls(&context, 1);
	assert(download_stream_next(stream, &data, &size, &result) == DOWNLOAD_WAITING);
	download_stream_stop(stream); // producer blocked in the HTTP read
	assert(context.cancelled);
	cleanup(&context);

	init(&context, CHUNK);
	fail_thread_create = true;
	assert(download_stream_start(&context) == NULL);
	cleanup(&context);
	puts("Passed: ordered data, ring wrap, partial reads, HTTP errors, full-queue cancellation, stalled-read cancellation, and thread creation failure.");
	return 0;
}
