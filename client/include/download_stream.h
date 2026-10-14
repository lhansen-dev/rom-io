#pragma once
#include <3ds.h>

typedef struct DownloadStream DownloadStream;
typedef Result (*DownloadStreamCheck)(httpcContext *context, void *arg);

enum { DOWNLOAD_WAITING, DOWNLOAD_DATA, DOWNLOAD_FINISHED };

DownloadStream *download_stream_start(httpcContext *context);
// Validate response headers in the producer, while the UI drains other streams.
DownloadStream *download_stream_start_checked(httpcContext *context, DownloadStreamCheck check, void *arg);
// Wait up to 100 ms for a chunk. Its data remains owned by the stream until
// release is called; only one consumer may hold a chunk at a time.
int download_stream_next(DownloadStream *stream, const u8 **data, u32 *size, Result *result);
// Nonblocking version for draining several streams fairly.
int download_stream_poll(DownloadStream *stream, const u8 **data, u32 *size, Result *result);
void download_stream_release(DownloadStream *stream);
// Wake the producer, cancel an outstanding HTTP read, and join before freeing.
void download_stream_stop(DownloadStream *stream);
