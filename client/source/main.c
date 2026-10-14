// rom-io 3DS client: browse the rom-io server over HTTP and download ROMs to the SD card.
#define _GNU_SOURCE // strcasestr
#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "download_stream.h"
#include "download_ranges.h"
#include "cia_install.h"

#define CFG_DIR "sdmc:/3ds/rom-io"
#define CFG_FILE CFG_DIR "/config.txt"
#define ROWS 24 // leave room for the title, filter, and controls
#define NAME_COLUMNS 38
#define WRITE_BUFFER_SIZE (512 * 1024)
#define DOWNLOAD_REDRAW_TICKS (SYSCLOCK_ARM11 / 4) // at most four updates per second

typedef struct { char *name; long long size; } Entry; // size < 0 means folder (name ends in '/')

static char server[64];                 // host:port
static char destdir[256] = "sdmc:/roms"; // last used download folder
static unsigned download_streams = 2;

static void present(void) { gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank(); }

static u32 wait_key(void) {
	u32 k = 0;
	while (!k) {
		if (!aptMainLoop()) return KEY_START;
		present();
		hidScanInput();
		k = hidKeysDown() | (hidKeysDownRepeat() & (KEY_UP | KEY_DOWN));
	}
	return k;
}

static void load_config(void) {
	FILE *f = fopen(CFG_FILE, "r");
	if (!f) return;
	if (fgets(server, sizeof server, f)) server[strcspn(server, "\r\n")] = 0;
	if (fgets(destdir, sizeof destdir, f)) destdir[strcspn(destdir, "\r\n")] = 0;
	fclose(f);
}

static void save_config(void) {
	mkdir(CFG_DIR, 0777);
	FILE *f = fopen(CFG_FILE, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n", server, destdir);
	fclose(f);
}

static bool keyboard(const char *hint, char *buf, size_t size) {
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, size - 1);
	swkbdSetHintText(&kb, hint);
	swkbdSetInitialText(&kb, buf);
	char out[256] = "";
	if (swkbdInputText(&kb, out, size < sizeof out ? size : sizeof out) != SWKBD_BUTTON_CONFIRM || !out[0]) return false;
	strcpy(buf, out);
	return true;
}

static void mkdirs(char *dir) {
	for (char *p = dir + 1; *p; p++)
		if (*p == '/') { *p = 0; mkdir(dir, 0777); *p = '/'; }
	mkdir(dir, 0777);
}

static bool http_create(httpcContext *c, const char *path, HTTPC_RequestMethod method) {
	char url[1024];
	int n = snprintf(url, sizeof url, "http://%s", server);
	for (const unsigned char *p = (const unsigned char *)path; *p && n < (int)sizeof url - 4; p++)
		n += (strchr("/-._~", *p) || (*p < 128 && isalnum(*p))) ? sprintf(url + n, "%c", *p) : sprintf(url + n, "%%%02X", *p);

	Result r = httpcOpenContext(c, method, url, 1);
	if (R_FAILED(r)) { printf("Bad URL %s\n", url); return false; }
	return true;
}

// Opens a GET to the server; on success the caller reads the body and closes the context.
static bool http_open(httpcContext *c, const char *path, u32 *size) {
	if (!http_create(c, path, HTTPC_METHOD_GET)) return false;
	u32 status = 0;
	Result r;
	if (R_FAILED(r = httpcBeginRequest(c)) || R_FAILED(r = httpcGetResponseStatusCode(c, &status))) {
		printf("Can't reach %s (%08lX)\n", server, (unsigned long)r);
	} else if (status != 200) {
		printf("Server said HTTP %lu\n", (unsigned long)status);
	} else if (R_SUCCEEDED(httpcGetDownloadSizeState(c, NULL, size))) {
		return true;
	}
	httpcCancelConnection(c);
	httpcCloseContext(c);
	return false;
}

static bool range_info(const char *remote, u32 *size, char *etag, size_t capacity) {
	httpcContext c;
	if (!http_create(&c, remote, HTTPC_METHOD_HEAD)) return false;
	u32 status = 0;
	char length[32], ranges[32];
	bool ok = R_SUCCEEDED(httpcBeginRequest(&c)) &&
		R_SUCCEEDED(httpcGetResponseStatusCode(&c, &status)) && status == 200 &&
		R_SUCCEEDED(httpcGetResponseHeader(&c, "Content-Length", length, sizeof length)) &&
		R_SUCCEEDED(httpcGetResponseHeader(&c, "Accept-Ranges", ranges, sizeof ranges)) &&
		!strcmp(ranges, "bytes") &&
		R_SUCCEEDED(httpcGetResponseHeader(&c, "ETag", etag, capacity));
	if (ok) {
		char *end;
		errno = 0;
		unsigned long long value = strtoull(length, &end, 10);
		ok = length[0] && !*end && !errno && value > 1 && value <= 0xFFFFFFFFULL;
		if (ok) *size = value;
	}
	if (!ok) httpcCancelConnection(&c);
	httpcCloseContext(&c);
	return ok;
}

typedef struct {
	u32 start, end, total;
	const char *etag;
} RangeCheck;

static bool http_range_prepare(httpcContext *c, const char *remote, const RangeCheck *range) {
	if (!http_create(c, remote, HTTPC_METHOD_GET)) return false;
	char header[64];
	snprintf(header, sizeof header, "bytes=%lu-%lu", (unsigned long)range->start, (unsigned long)range->end);
	bool ok = R_SUCCEEDED(httpcAddRequestHeaderField(c, "Range", header)) &&
		R_SUCCEEDED(httpcAddRequestHeaderField(c, "If-Range", range->etag));
	if (!ok) {
		printf("Can't start range request\n");
		httpcCancelConnection(c);
		httpcCloseContext(c);
	}
	return ok;
}

static Result http_range_check(httpcContext *c, void *arg) {
	const RangeCheck *range = arg;
	char expected[96], actual[96], response_etag[128];
	snprintf(expected, sizeof expected, "bytes %lu-%lu/%lu", (unsigned long)range->start,
		(unsigned long)range->end, (unsigned long)range->total);
	u32 status = 0, length = 0;
	Result r = httpcBeginRequest(c);
	if (R_FAILED(r)) return r;
	// The timed status command can time out despite a server response on real
	// hardware. Keep the original status call in this worker; the consumer
	// monitors aggregate progress and cancels stalled requests independently.
	r = httpcGetResponseStatusCode(c, &status);
	if (R_FAILED(r)) return r;
	bool ok = status == 206 &&
		R_SUCCEEDED(httpcGetResponseHeader(c, "Content-Range", actual, sizeof actual)) &&
		!strcmp(actual, expected) &&
		R_SUCCEEDED(httpcGetResponseHeader(c, "ETag", response_etag, sizeof response_etag)) &&
		!strcmp(response_etag, range->etag) &&
		R_SUCCEEDED(httpcGetDownloadSizeState(c, NULL, &length)) && length == range->end - range->start + 1;
	return ok ? 0 : -1;
}

static char *fetch_listing(const char *path) {
	httpcContext c;
	u32 size, got = 0;
	if (!http_open(&c, path, &size)) return NULL;
	char *buf = malloc(size + 1);
	Result r = size ? httpcDownloadData(&c, (u8 *)buf, size, &got) : 0;
	httpcCloseContext(&c);
	if (R_FAILED(r) || got != size) { free(buf); return NULL; }
	buf[size] = 0;
	return buf;
}

typedef struct {
	u64 started, sample_tick, sample_bytes;
	double peak;
	bool drawn;
} DownloadStats;

static void download_progress(u64 done, u32 size, DownloadStats *stats, u64 now, bool final) {
	u64 interval = now - stats->sample_tick;
	// Sample aggregate throughput over at least a second, rather than letting
	// short bursts from queued chunks produce misleading peaks.
	if (interval >= SYSCLOCK_ARM11 || (final && !stats->peak && interval)) {
		double rate = (done - stats->sample_bytes) * (double)SYSCLOCK_ARM11 / interval;
		if (rate > stats->peak) stats->peak = rate;
		stats->sample_tick = now;
		stats->sample_bytes = done;
	}
	double seconds = (now - stats->started) / (double)SYSCLOCK_ARM11;
	double bytes_per_second = seconds > 0 ? done / seconds : 0;
	u64 remaining = done < size ? size - done : 0;
	u64 eta = bytes_per_second > 0 ? (u64)(remaining / bytes_per_second) : 0;
	if (stats->drawn) printf("\033[1A");
	printf("\r\033[2K%lu/%lu MiB  %.2f MiB/s", (unsigned long)(done / 1048576),
		(unsigned long)(size / 1048576), bytes_per_second / 1048576.0);
	if (bytes_per_second > 0)
		printf("  ETA %llu:%02llu", (unsigned long long)(eta / 60), (unsigned long long)(eta % 60));
	printf("\n\r\033[2KMax throughput: %.2f MiB/s", stats->peak / 1048576.0);
	stats->drawn = true;
	// Download progress does not need to wait for a display frame.
	gfxFlushBuffers();
	gfxSwapBuffers();
}

// Streams to "<dest>.part" and renames on success so a failed download never clobbers a good file.
static bool download_single(const char *remote, const char *dest) {
	printf("Using 1 stream\n");
	static char writebuf[WRITE_BUFFER_SIZE];
	char part[600];
	snprintf(part, sizeof part, "%s.part", dest);
	httpcContext c;
	u32 size;
	u64 done = 0;
	if (!http_open(&c, remote, &size)) return false;
	FILE *f = fopen(part, "wb");
	if (!f) { printf("Can't create %s\n", part); httpcCancelConnection(&c); httpcCloseContext(&c); return false; }
	if (setvbuf(f, writebuf, _IOFBF, sizeof writebuf) != 0) {
		printf("Can't buffer SD writes\n");
		httpcCancelConnection(&c);
		httpcCloseContext(&c);
		fclose(f);
		remove(part);
		return false;
	}

	DownloadStream *stream = download_stream_start(&c);
	if (!stream) {
		printf("Can't start download worker\n");
		httpcCancelConnection(&c);
		httpcCloseContext(&c);
		fclose(f);
		remove(part);
		return false;
	}
	bool ok = true;
	Result r = 0;
	u64 started = svcGetSystemTick(), last_redraw = started;
	DownloadStats stats = { .started = started, .sample_tick = started };
	download_progress(0, size, &stats, started, false);
	for (;;) {
		const u8 *buf;
		u32 got;
		int state = download_stream_next(stream, &buf, &got, &r);
		if (state == DOWNLOAD_FINISHED) break;
		if (state == DOWNLOAD_DATA) {
			bool written = fwrite(buf, 1, got, f) == got;
			download_stream_release(stream);
			if (!written) { printf("\nWrite failed (SD full?)\n"); ok = false; break; }
			done += got;
		}
		u64 now = svcGetSystemTick();
		if (now - last_redraw >= DOWNLOAD_REDRAW_TICKS) {
			download_progress(done, size, &stats, now, false);
			last_redraw = now;
		}
		hidScanInput();
		if (!aptMainLoop() || (hidKeysDown() & KEY_B)) { printf("\nCancelled\n"); ok = false; break; }
	}
	download_stream_stop(stream);
	if (ok && (R_FAILED(r) || done != size)) { printf("\nDownload error %08lX\n", (unsigned long)r); ok = false; }
	if (!ok) httpcCancelConnection(&c);
	httpcCloseContext(&c);
	if (fclose(f) != 0 && ok) { printf("\nWrite failed\n"); ok = false; }
	if (ok) download_progress(done, size, &stats, svcGetSystemTick(), true);
	if (ok) {
		remove(dest);
		if (rename(part, dest) != 0) { printf("\nRename to %s failed\n", dest); ok = false; }
	}
	if (!ok) remove(part);
	return ok;
}

typedef struct {
	Handle file;
	u32 size;
	u64 allocated;
	LightLock lock;
	bool cancelled, finished;
	Result result;
} FileAllocation;

static void allocate_file(void *arg) {
	FileAllocation *allocation = arg;
	Result r = FSFILE_SetSize(allocation->file, 0);
	u64 allocated = 0;
	while (R_SUCCEEDED(r) && allocated < allocation->size) {
		LightLock_Lock(&allocation->lock);
		bool cancelled = allocation->cancelled;
		LightLock_Unlock(&allocation->lock);
		if (cancelled) break;
		u64 next = allocated + 32 * 1024 * 1024;
		if (next > allocation->size) next = allocation->size;
		r = FSFILE_SetSize(allocation->file, next);
		if (R_SUCCEEDED(r)) {
			allocated = next;
			LightLock_Lock(&allocation->lock);
			allocation->allocated = allocated;
			LightLock_Unlock(&allocation->lock);
		}
	}
	LightLock_Lock(&allocation->lock);
	allocation->result = r;
	allocation->finished = true;
	LightLock_Unlock(&allocation->lock);
}

static bool prepare_file(Handle file, u32 size) {
	FileAllocation allocation = { .file = file, .size = size };
	LightLock_Init(&allocation.lock);
	Thread worker = threadCreate(allocate_file, &allocation, 16 * 1024, 0x30, -2, false);
	if (!worker) { printf("Can't start SD allocation worker\n"); return false; }
	u64 last_redraw = 0;
	for (;;) {
		LightLock_Lock(&allocation.lock);
		u64 allocated = allocation.allocated;
		bool finished = allocation.finished, cancelled = allocation.cancelled;
		LightLock_Unlock(&allocation.lock);
		u64 now = svcGetSystemTick();
		if (now - last_redraw >= DOWNLOAD_REDRAW_TICKS || finished) {
			printf("\r\033[2KAllocating SD space: %lu/%lu MiB", (unsigned long)(allocated / 1048576),
				(unsigned long)(size / 1048576));
			printf("\n\r\033[2K%s", cancelled ? "Cancelling SD allocation..." : "B: cancel");
			gfxFlushBuffers(); gfxSwapBuffers();
			last_redraw = now;
			if (!finished) printf("\033[1A");
		}
		if (finished) break;
		hidScanInput();
		if (!aptMainLoop() || (hidKeysDown() & KEY_B)) {
			LightLock_Lock(&allocation.lock);
			allocation.cancelled = true;
			LightLock_Unlock(&allocation.lock);
		}
		svcSleepThread(16000000);
	}
	threadJoin(worker, U64_MAX);
	threadFree(worker);
	printf("\n");
	if (allocation.cancelled) { printf("Cancelled\n"); return false; }
	if (R_FAILED(allocation.result)) {
		printf("Can't allocate file (SD full?): %08lX\n", (unsigned long)allocation.result);
		return false;
	}
	return true;
}

static bool download_parallel(const char *remote, const char *dest, u32 size, const char *etag, unsigned count) {
	char part[600];
	snprintf(part, sizeof part, "%s.part", dest);
	const char *sdpath = !strncmp(part, "sdmc:", 5) ? part + 5 : part;
	u16 utf16[600];
	ssize_t units = utf8_to_utf16(utf16, (const u8 *)sdpath, 599);
	if (units < 0 || units > 599) { printf("Invalid SD path\n"); return false; }
	utf16[units] = 0;
	Handle file;
	Result r = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
		fsMakePath(PATH_UTF16, utf16), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
	if (R_FAILED(r)) { printf("Can't create file (%08lX)\n", (unsigned long)r); return false; }
	printf("Preparing file for %u streams...\n", count);
	present();
	if (!prepare_file(file, size)) {
		FSFILE_Close(file);
		remove(part);
		return false;
	}

	httpcContext contexts[MAX_DOWNLOAD_STREAMS];
	RangeCheck ranges[MAX_DOWNLOAD_STREAMS];
	DownloadStream *streams[MAX_DOWNLOAD_STREAMS] = {0};
	u32 start[MAX_DOWNLOAD_STREAMS], length[MAX_DOWNLOAD_STREAMS];
	u64 written[MAX_DOWNLOAD_STREAMS] = {0}, done = 0;
	bool finished[MAX_DOWNLOAD_STREAMS] = {0}, ok = true;
	bool context_open[MAX_DOWNLOAD_STREAMS] = {0};
	unsigned remaining = count;
	int opened = 0;
	printf("Connecting %u streams...\n", count);
	present();
	for (unsigned i = 0; i < count; i++) {
		download_range(size, i, count, &start[i], &length[i]);
		ranges[i] = (RangeCheck){ start[i], start[i] + length[i] - 1, size, etag };
		if (!http_range_prepare(&contexts[i], remote, &ranges[i])) { ok = false; break; }
		opened++;
		context_open[i] = true;
		streams[i] = download_stream_start_checked(&contexts[i], http_range_check, &ranges[i]);
		if (!streams[i]) { printf("Can't start download worker\n"); ok = false; break; }
	}
	u64 started = svcGetSystemTick(), last_redraw = started;
	u64 last_activity = started;
	DownloadStats stats = { .started = started, .sample_tick = started };
	if (ok) download_progress(0, size, &stats, started, false);
	while (ok && remaining) {
		bool active = false;
		for (unsigned i = 0; i < count && ok; i++) {
			if (finished[i]) continue;
			const u8 *data;
			u32 got;
			int state = download_stream_poll(streams[i], &data, &got, &r);
			if (state == DOWNLOAD_FINISHED) {
				finished[i] = true;
				remaining--;
				if (R_FAILED(r) || written[i] != length[i]) {
					printf("\nStream %u failed (%08lX)\n", i + 1, (unsigned long)r);
					ok = false;
				} else {
					// Release a completed request immediately so any request
					// queued by the HTTP service can start using its slot.
					download_stream_stop(streams[i]);
					streams[i] = NULL;
					httpcCloseContext(&contexts[i]);
					context_open[i] = false;
				}
			} else if (state == DOWNLOAD_DATA) {
				active = true;
				u32 saved = 0;
				if (written[i] + got > length[i]) {
					printf("\nStream exceeded its requested range\n");
					ok = false;
				} else {
					// Native writes take 64-bit offsets, including beyond 2 GiB.
					r = FSFILE_Write(file, &saved, start[i] + written[i], data, got, 0);
					if (R_FAILED(r) || saved != got) { printf("\nWrite failed (SD full?)\n"); ok = false; }
				}
				download_stream_release(streams[i]);
				written[i] += saved;
				done += saved;
			}
		}
		u64 now = svcGetSystemTick();
		if (active) last_activity = now;
		if (ok && remaining && now - last_activity >= 30ULL * SYSCLOCK_ARM11) {
			printf("\nNo download progress for 30 seconds.\n");
			ok = false;
		}
		if (now - last_redraw >= DOWNLOAD_REDRAW_TICKS) {
			download_progress(done, size, &stats, now, false);
			last_redraw = now;
		}
		hidScanInput();
		if (!aptMainLoop() || (hidKeysDown() & KEY_B)) { printf("\nCancelled\n"); ok = false; }
		if (!active && ok) svcSleepThread(5000000);
	}
	// Cancel all requests before joining any worker, including workers still
	// waiting for their response headers.
	if (!ok) {
		for (int i = 0; i < opened; i++)
			if (context_open[i]) httpcCancelConnection(&contexts[i]);
	}
	for (int i = 0; i < opened; i++) {
		if (!context_open[i]) continue;
		if (streams[i]) download_stream_stop(streams[i]);
		if (!ok || !streams[i]) httpcCancelConnection(&contexts[i]);
		httpcCloseContext(&contexts[i]);
	}
	if (R_FAILED(FSFILE_Flush(file))) { printf("\nSD flush failed\n"); ok = false; }
	if (R_FAILED(FSFILE_Close(file))) { printf("\nSD close failed\n"); ok = false; }
	if (ok) {
		download_progress(done, size, &stats, svcGetSystemTick(), true);
		remove(dest);
		if (rename(part, dest) != 0) { printf("\nRename failed\n"); ok = false; }
	}
	if (!ok) remove(part);
	return ok;
}

static bool download(const char *remote, const char *dest) {
	u32 size;
	char etag[128];
	bool absolute_sd = !strncmp(dest, "sdmc:/", 6) || dest[0] == '/';
	if (download_streams > 1 && absolute_sd && range_info(remote, &size, etag, sizeof etag) && size >= 1024 * 1024)
		return download_parallel(remote, dest, size, etag, download_streams);
	return download_single(remote, dest);
}

static DownloadStats install_stats;
static u64 install_redraw;

static bool confirm_install(const AM_TitleInfo *info, u64 required) {
	consoleClear();
	printf("Install CIA to SD?\n\nTitle: %016llX\nVersion: %u\nSpace needed: %llu MiB\n\n"
		"An existing title may be replaced.\nThe downloaded CIA will be kept.\n\nA: install   B: cancel\n",
		(unsigned long long)info->titleID, info->version,
		(unsigned long long)((required + 1048575) / 1048576));
	for (;;) {
		u32 key = wait_key();
		if (key & (KEY_B | KEY_START)) return false;
		if (key & KEY_A) break;
	}
	consoleClear();
	printf("Installing CIA...\n\nB: cancel\n\n");
	u64 now = svcGetSystemTick();
	install_stats = (DownloadStats){ .started = now, .sample_tick = now };
	install_redraw = now;
	return true;
}

static bool install_progress(u64 done, u64 total) {
	u64 now = svcGetSystemTick();
	if (!done || done == total || now - install_redraw >= DOWNLOAD_REDRAW_TICKS) {
		download_progress(done, (u32)total, &install_stats, now, done == total);
		install_redraw = now;
	}
	hidScanInput();
	return aptMainLoop() && !(hidKeysDown() & KEY_B);
}

static void download_flow(const char *path, const char *name, long long size) {
	bool install = false;
	size_t name_len = strlen(name);
	if (name_len >= 4 && strcasecmp(name + name_len - 4, ".cia") == 0) {
		consoleClear();
		printf("%s\n\nA: download only\nX: download then install\nB: cancel\n", name);
		for (;;) {
			u32 key = wait_key();
			if (key & (KEY_B | KEY_START)) return;
			if (key & KEY_X) { install = true; break; }
			if (key & KEY_A) break;
		}
		if (install) {
			Result r = amInit();
			if (R_FAILED(r)) {
				printf("\nCan't access installer (%08lX).\nRun under Luma3DS CFW.\n\nPress any button", (unsigned long)r);
				wait_key();
				return;
			}
			amExit();
		}
	}
	if (size > 0xFFFFFFFFLL) {
		consoleClear();
		printf("%s\n\nFile is too large for the SD card.\n"
			"FAT32 files must be smaller than 4 GiB.\n\n"
			"Trim this ROM or convert it to a smaller\n"
			"CIA on your computer first.\n\nPress any button", name);
		wait_key();
		return;
	}
	char dir[256];
	strcpy(dir, destdir);
	if (!keyboard("Save to folder (e.g. sdmc:/roms/gba)", dir, sizeof dir)) return;
	if (install && strncmp(dir, "sdmc:/", 6) != 0) {
		consoleClear();
		printf("Installation needs an absolute sdmc:/\nfolder, e.g. sdmc:/cias.\n\nPress any button");
		wait_key();
		return;
	}
	size_t len = strlen(dir);
	while (len > 1 && dir[len - 1] == '/') dir[--len] = 0;
	mkdirs(dir);

	char dest[512], remote[1024];
	snprintf(dest, sizeof dest, "%s/%s", dir, name);
	snprintf(remote, sizeof remote, "%s%s", path, name);
	consoleClear();
	struct stat st;
	if (stat(dest, &st) == 0) {
		if (!S_ISREG(st.st_mode)) {
			printf("Destination is not a regular file:\n%s\n\nPress any button", dest);
			wait_key();
			return;
		}
		printf("File already exists:\n%s\n\n"
			"Overwrite the original?\n\nA: overwrite   B: cancel\n", dest);
		for (;;) {
			u32 key = wait_key();
			if (key & (KEY_B | KEY_START)) return;
			if (key & KEY_A) break;
		}
	} else if (errno != ENOENT) {
		printf("Can't check destination:\n%s\n%s\n\nPress any button", dest, strerror(errno));
		wait_key();
		return;
	}
	consoleClear();
	printf("Downloading %s\nto %s\n\nB: cancel\n\n", name, dir);
	if (download(remote, dest)) {
		strcpy(destdir, dir);
		save_config();
		if (install) {
			const char *error;
			Result result;
			if (cia_install(dest, confirm_install, install_progress, &error, &result))
				printf("\nInstalled! CIA kept at:\n%s", dest);
			else {
				printf("\n%s\n", error);
				if (R_FAILED(result)) printf("Error: %08lX\n", (unsigned long)result);
				printf("Downloaded CIA kept at:\n%s", dest);
			}
		} else printf("\nDone!");
	}
	printf("\n\nPress any button");
	wait_key();
}

static void fmt_size(char *out, long long b) {
	if (b < 0) strcpy(out, "");
	else if (b < 1024 * 1024) sprintf(out, "%lluK", (b + 1023) / 1024);
	else if (b < 1024LL * 1024 * 1024) sprintf(out, "%.1fM", b / 1048576.0);
	else sprintf(out, "%.2fG", b / 1073741824.0);
}

// Copies the entries whose names contain every space-separated word of filter, ignoring case.
static int apply_filter(const Entry *all, int n, const char *filter, Entry *out) {
	int m = 0;
	for (int i = 0; i < n; i++) {
		char words[64];
		strcpy(words, filter);
		bool ok = true;
		for (char *w = strtok(words, " "); w && ok; w = strtok(NULL, " ")) ok = strcasestr(all[i].name, w);
		if (ok) out[m++] = all[i];
	}
	return m;
}

static int entry_rows(const Entry *entry) {
	size_t length = strlen(entry->name);
	return length ? (length + NAME_COLUMNS - 1) / NAME_COLUMNS : 1;
}

static void draw_entry(const Entry *entry, bool selected) {
	char size[16];
	fmt_size(size, entry->size);
	const char *name = entry->name;
	bool first = true;
	do {
		printf("%c %-*.*s %8s\n", first && selected ? '>' : ' ',
			NAME_COLUMNS, NAME_COLUMNS, name, first ? size : "");
		size_t length = strlen(name);
		name += length < NAME_COLUMNS ? length : NAME_COLUMNS;
		first = false;
	} while (*name);
}

// Draws the listing and handles scrolling; returns the action key that left the menu.
static u32 menu(const char *path, const char *filter, Entry *e, int n, int *cur) {
	for (;;) {
		consoleClear();
		printf("%.49s\n%.49s\n", server, path);
		if (filter[0]) printf("Filter: %.41s\n", filter);
		// Count physical rows so wrapped names do not scroll the controls off
		// screen. Start near the selection and show only complete entries.
		int top = n ? *cur : 0, used = n ? entry_rows(&e[top]) : 0;
		while (top > 0 && used + entry_rows(&e[top - 1]) <= ROWS / 2) {
			used += entry_rows(&e[--top]);
		}
		int end = top;
		used = 0;
		while (end < n && used + entry_rows(&e[end]) <= ROWS)
			used += entry_rows(&e[end++]);
		if (end == n) {
			while (top > 0 && used + entry_rows(&e[top - 1]) <= ROWS)
				used += entry_rows(&e[--top]);
		}
		for (int i = top; i < end; i++) draw_entry(&e[i], i == *cur);
		int page = end - top;
		if (!n) printf("  (empty)\n");
		printf("\nA:open B:back Y:filter X:server\nL:streams=%u R:refresh START:quit", download_streams);

		u32 k = wait_key();
		if (n && (k & KEY_DOWN)) *cur = (*cur + 1) % n;
		else if (n && (k & KEY_UP)) *cur = (*cur + n - 1) % n;
		else if (n && (k & KEY_RIGHT)) *cur = *cur + page < n ? *cur + page : n - 1;
		else if (n && (k & KEY_LEFT)) *cur = *cur > page ? *cur - page : 0;
		else if (k & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_START)) return k;
	}
}

int main(void) {
	osSetSpeedupEnable(true);
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	httpcInit(0);
	load_config();
	if (!server[0]) {
		if (!keyboard("Server IP:port, e.g. 192.168.1.10:8765", server, sizeof server)) goto quit;
		save_config();
	}

	char path[1024] = "/", filter[64] = "";
	char selected[512] = "";
	for (;;) {
		consoleClear();
		char *list = fetch_listing(path);
		if (!list) {
			printf("\nCouldn't load %s\n\nA: retry   X: change server   START: quit\n", path);
			u32 k = wait_key();
			if (k & KEY_START) break;
			if ((k & KEY_X) && keyboard("Server IP:port", server, sizeof server)) {
				save_config(); strcpy(path, "/"); filter[0] = 0; selected[0] = 0;
			}
			continue;
		}

		int total = 0, max = 1;
		for (char *p = list; *p; p++) max += *p == '\n';
		Entry *all = calloc(max, sizeof *all), *e = calloc(max, sizeof *e); // e = entries shown after filtering
		for (char *line = strtok(list, "\n"); line; line = strtok(NULL, "\n")) {
			char *tab = strchr(line, '\t');
			all[total] = (Entry){ line, -1 };
			if (tab) { *tab = 0; all[total].size = strtoll(tab + 1, NULL, 10); }
			total++;
		}
		int n = apply_filter(all, total, filter, e);

		int cur = 0;
		for (int i = 0; selected[0] && i < n; i++)
			if (!strcmp(e[i].name, selected)) { cur = i; break; }
		selected[0] = 0;
		bool quit = false, reload = false;
		while (!quit && !reload) {
			u32 k = menu(path, filter, e, n, &cur);
			if (k & KEY_START) quit = true;
			else if (k & KEY_L) download_streams = download_streams % MAX_DOWNLOAD_STREAMS + 1;
			else if (k & KEY_R) {
				if (n) snprintf(selected, sizeof selected, "%s", e[cur].name);
				reload = true;
			} else if (k & KEY_Y) {
				if (keyboard("Filter: words to match", filter, sizeof filter)) { n = apply_filter(all, total, filter, e); cur = 0; }
			} else if ((k & KEY_B) && filter[0]) {
				filter[0] = 0;
				n = apply_filter(all, total, filter, e);
				cur = 0;
			} else if (k & KEY_X) {
				if (keyboard("Server IP:port", server, sizeof server)) { save_config(); strcpy(path, "/"); filter[0] = 0; reload = true; }
			} else if (k & KEY_B) {
				if (strcmp(path, "/")) { // drop the last "name/" component
					path[strlen(path) - 1] = 0;
					strrchr(path, '/')[1] = 0;
					filter[0] = 0;
					reload = true;
				}
			} else if ((k & KEY_A) && n) {
				if (e[cur].size < 0) {
					if (strlen(path) + strlen(e[cur].name) < sizeof path) { strcat(path, e[cur].name); filter[0] = 0; reload = true; }
				} else {
					download_flow(path, e[cur].name, e[cur].size);
				}
			}
		}
		free(all);
		free(e);
		free(list);
		if (quit) break;
	}

quit:
	httpcExit();
	gfxExit();
	return 0;
}
