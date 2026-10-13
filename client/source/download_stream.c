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

	LightLock_Lock(&stream->lock);
	stream->result = result;
	stream->finished = true;
	CondVar_Broadcast(&stream->changed);
	LightLock_Unlock(&stream->lock);
}

DownloadStream *download_stream_start(httpcContext *context) {
	DownloadStream *stream = calloc(1, sizeof *stream);
	if (!stream) return NULL;
	stream->context = context;
	LightLock_Init(&stream->lock);
	CondVar_Init(&stream->changed);
	stream->thread = threadCreate(receive, stream, 16 * 1024, 0x30, -2, false);
	if (!stream->thread) { free(stream); return NULL; }
	return stream;
}

int download_stream_next(DownloadStream *stream, const u8 **data, u32 *size, Result *result) {
	LightLock_Lock(&stream->lock);
	if (!stream->count && !stream->finished)
		CondVar_WaitTimeout(&stream->changed, &stream->lock, 100000000);
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
