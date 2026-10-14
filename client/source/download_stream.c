#include "download_stream.h"
#include <stdlib.h>

#define CHUNK_SIZE (256 * 1024)
#define CHUNK_COUNT 4

struct DownloadStream {
	httpcContext *context;
	Thread thread;
	LightLock lock;
	CondVar changed;
	bool stopped, finished;
	Result result;
	DownloadStreamCheck check;
	void *check_arg;
	unsigned head, count;
	struct {
		u8 data[CHUNK_SIZE];
		u32 size;
	} chunks[CHUNK_COUNT];
};

static void receive(void *arg) {
	DownloadStream *stream = arg;
	unsigned tail = 0;
	Result result = 0;
	if (stream->check) {
		result = stream->check(stream->context, stream->check_arg);
		if (result != 0) goto finished;
	}
	do {
		LightLock_Lock(&stream->lock);
		while (stream->count == CHUNK_COUNT && !stream->stopped)
			CondVar_Wait(&stream->changed, &stream->lock);
		bool stopped = stream->stopped;
		LightLock_Unlock(&stream->lock);
		if (stopped) break;

		// The producer owns this slot until it publishes the chunk under the
		// lock. The consumer keeps counted slots occupied throughout SD writes.
		u32 got = 0;
		result = httpcDownloadData(stream->context, stream->chunks[tail].data, CHUNK_SIZE, &got);
		LightLock_Lock(&stream->lock);
		if (got && !stream->stopped) {
			stream->chunks[tail].size = got;
			stream->count++;
			tail = (tail + 1) % CHUNK_COUNT;
		}
		stopped = stream->stopped;
		CondVar_Broadcast(&stream->changed);
		LightLock_Unlock(&stream->lock);
		if (stopped) break;
	} while (result == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);

finished:
	LightLock_Lock(&stream->lock);
	stream->result = result;
	stream->finished = true;
	CondVar_Broadcast(&stream->changed);
	LightLock_Unlock(&stream->lock);
}

DownloadStream *download_stream_start_checked(httpcContext *context, DownloadStreamCheck check, void *arg) {
	DownloadStream *stream = calloc(1, sizeof *stream);
	if (!stream) return NULL;
	stream->context = context;
	stream->check = check;
	stream->check_arg = arg;
	LightLock_Init(&stream->lock);
	CondVar_Init(&stream->changed);
	stream->thread = threadCreate(receive, stream, 16 * 1024, 0x30, -2, false);
	if (!stream->thread) { free(stream); return NULL; }
	return stream;
}

DownloadStream *download_stream_start(httpcContext *context) {
	return download_stream_start_checked(context, NULL, NULL);
}

static int next(DownloadStream *stream, const u8 **data, u32 *size, Result *result, s64 timeout) {
	LightLock_Lock(&stream->lock);
	if (timeout && !stream->count && !stream->finished)
		CondVar_WaitTimeout(&stream->changed, &stream->lock, timeout);
	int state = DOWNLOAD_WAITING;
	if (stream->count) {
		*data = stream->chunks[stream->head].data;
		*size = stream->chunks[stream->head].size;
		state = DOWNLOAD_DATA;
	} else if (stream->finished) {
		*result = stream->result;
		state = DOWNLOAD_FINISHED;
	}
	LightLock_Unlock(&stream->lock);
	return state;
}

int download_stream_next(DownloadStream *stream, const u8 **data, u32 *size, Result *result) {
	return next(stream, data, size, result, 100000000);
}

int download_stream_poll(DownloadStream *stream, const u8 **data, u32 *size, Result *result) {
	return next(stream, data, size, result, 0);
}

void download_stream_release(DownloadStream *stream) {
	LightLock_Lock(&stream->lock);
	stream->head = (stream->head + 1) % CHUNK_COUNT;
	stream->count--;
	CondVar_Broadcast(&stream->changed);
	LightLock_Unlock(&stream->lock);
}

void download_stream_stop(DownloadStream *stream) {
	LightLock_Lock(&stream->lock);
	stream->stopped = true;
	bool finished = stream->finished;
	CondVar_Broadcast(&stream->changed);
	LightLock_Unlock(&stream->lock);
	if (!finished) httpcCancelConnection(stream->context);
	threadJoin(stream->thread, U64_MAX);
	threadFree(stream->thread);
	free(stream);
}
