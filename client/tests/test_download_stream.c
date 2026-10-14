#define _POSIX_C_SOURCE 200809L
#include "download_stream.h"
#include "download_ranges.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	for (u32 i = 0; i < *got; i++) buffer[i] = pattern(context->base + context->offset + i);
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

static void check_parallel(unsigned count) {
	const u64 length = 13 * CHUNK + 19;
	u8 *output = malloc(length);
	assert(output);
	httpcContext contexts[MAX_DOWNLOAD_STREAMS];
	DownloadStream *streams[MAX_DOWNLOAD_STREAMS];
	u64 written[MAX_DOWNLOAD_STREAMS] = {0};
	bool finished[MAX_DOWNLOAD_STREAMS] = {0};
	unsigned remaining = count;
	for (unsigned i = 0; i < count; i++) {
		u32 start, size;
		download_range(length, i, count, &start, &size);
		init(&contexts[i], size);
		contexts[i].base = start;
		streams[i] = download_stream_start(&contexts[i]);
		assert(streams[i]);
	}
	while (remaining) {
		bool active = false;
		for (unsigned i = 0; i < count; i++) {
			if (finished[i]) continue;
			const u8 *data;
			u32 size;
			Result result;
			int state = download_stream_poll(streams[i], &data, &size, &result);
			if (state == DOWNLOAD_DATA) {
				assert(written[i] + size <= contexts[i].length);
				memcpy(output + contexts[i].base + written[i], data, size);
				written[i] += size;
				download_stream_release(streams[i]);
				active = true;
			} else if (state == DOWNLOAD_FINISHED) {
				assert(result == 0);
				assert(written[i] == contexts[i].length);
				finished[i] = true;
				remaining--;
			}
		}
		if (!active) { struct timespec delay = { .tv_nsec = 1000000 }; nanosleep(&delay, NULL); }
	}
	for (u64 i = 0; i < length; i++) assert(output[i] == pattern(i));
	for (unsigned i = 0; i < count; i++) { download_stream_stop(streams[i]); cleanup(&contexts[i]); }
	free(output);
}

static Result reject_response(httpcContext *context, void *arg) {
	(void)context; (void)arg;
	return READ_ERROR;
}

static Result wait_response(httpcContext *context, void *arg) {
	(void)arg;
	LightLock_Lock(&context->lock);
	context->calls++;
	CondVar_Broadcast(&context->changed);
	while (!context->cancelled) CondVar_Wait(&context->changed, &context->lock);
	LightLock_Unlock(&context->lock);
	return READ_ERROR;
}

static void check_startup(void) {
	httpcContext slow, ready;
	init(&slow, CHUNK);
	init(&ready, CHUNK);
	DownloadStream *waiting = download_stream_start_checked(&slow, wait_response, NULL);
	DownloadStream *reading = download_stream_start(&ready);
	assert(waiting && reading);
	wait_calls(&slow, 1);
	const u8 *data;
	u32 size;
	Result result;
	assert(download_stream_poll(waiting, &data, &size, &result) == DOWNLOAD_WAITING);
	// A response stalled during startup must not prevent another stream from
	// delivering data to the consumer, or prevent cancellation and joining.
	int state;
	do { state = download_stream_next(reading, &data, &size, &result); } while (state == DOWNLOAD_WAITING);
	assert(state == DOWNLOAD_DATA && size == CHUNK);
	for (u32 i = 0; i < size; i++) assert(data[i] == pattern(i));
	download_stream_release(reading);
	download_stream_stop(waiting);
	download_stream_stop(reading);
	assert(slow.cancelled);
	cleanup(&slow);
	cleanup(&ready);

	init(&slow, CHUNK);
	waiting = download_stream_start_checked(&slow, reject_response, NULL);
	assert(waiting);
	do { state = download_stream_next(waiting, &data, &size, &result); } while (state == DOWNLOAD_WAITING);
	assert(state == DOWNLOAD_FINISHED && result == READ_ERROR);
	assert(slow.calls == 0); // never read a body whose headers failed validation
	download_stream_stop(waiting);
	cleanup(&slow);
}

int main(void) {
	// Empty, partial, aligned, and many wraps of the ring buffer.
	check_transfer(0, U64_MAX);
	check_transfer(17, U64_MAX);
	check_transfer(CHUNK, U64_MAX);
	check_transfer(37 * CHUNK + 17, U64_MAX);
	// An HTTP error may arrive with a partial final chunk.
	check_transfer(10 * CHUNK, CHUNK + 17);
	check_startup();
	for (unsigned count = 2; count <= MAX_DOWNLOAD_STREAMS; count++) {
		check_parallel(count);
		// Check the same range calculation near FAT32's maximum file size.
		u64 covered = 0;
		for (unsigned i = 0; i < count; i++) {
			u32 start, length;
			download_range(UINT32_MAX, i, count, &start, &length);
			assert(start == covered);
			assert(length > 0);
			covered += length;
		}
		assert(covered == UINT32_MAX);
	}

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
	assert(download_stream_poll(stream, &data, &size, &result) == DOWNLOAD_WAITING);
	assert(download_stream_next(stream, &data, &size, &result) == DOWNLOAD_WAITING);
	download_stream_stop(stream); // producer blocked in the HTTP read
	assert(context.cancelled);
	cleanup(&context);

	init(&context, CHUNK);
	fail_thread_create = true;
	assert(download_stream_start(&context) == NULL);
	cleanup(&context);
	puts("Passed: ordered data, ring wrap, partial reads, 2/3/4 streams, large-file ranges, stalled startup, rejected headers, HTTP errors, cancellation, and thread creation failure.");
	return 0;
}
