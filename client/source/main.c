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

#define CFG_DIR "sdmc:/3ds/rom-io"
#define CFG_FILE CFG_DIR "/config.txt"
#define ROWS 24 // leave room for the title, filter, and controls
#define NAME_COLUMNS 38
#define WRITE_BUFFER_SIZE (512 * 1024)
#define DOWNLOAD_REDRAW_TICKS (SYSCLOCK_ARM11 / 4) // at most four updates per second

typedef struct { char *name; long long size; } Entry; // size < 0 means folder (name ends in '/')

static char server[64];                 // host:port
static char destdir[256] = "sdmc:/roms"; // last used download folder

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

// Opens a GET to the server; on success the caller reads the body and closes the context.
static bool http_open(httpcContext *c, const char *path, u32 *size) {
	char url[1024];
	int n = snprintf(url, sizeof url, "http://%s", server);
	for (const unsigned char *p = (const unsigned char *)path; *p && n < (int)sizeof url - 4; p++)
		n += (strchr("/-._~", *p) || (*p < 128 && isalnum(*p))) ? sprintf(url + n, "%c", *p) : sprintf(url + n, "%%%02X", *p);

	u32 status = 0;
	Result r = httpcOpenContext(c, HTTPC_METHOD_GET, url, 1);
	if (R_FAILED(r)) { printf("Bad URL %s\n", url); return false; }
	if (R_FAILED(r = httpcBeginRequest(c)) || R_FAILED(r = httpcGetResponseStatusCode(c, &status))) {
		printf("Can't reach %s (%08lX)\n", server, (unsigned long)r);
	} else if (status != 200) {
		printf("Server said HTTP %lu\n", (unsigned long)status);
	} else if (R_SUCCEEDED(httpcGetDownloadSizeState(c, NULL, size))) {
		return true;
	}
	httpcCloseContext(c);
	return false;
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

static void download_progress(u64 done, u32 size, u64 started, u64 now) {
	double seconds = (now - started) / (double)SYSCLOCK_ARM11;
	double bytes_per_second = seconds > 0 ? done / seconds : 0;
	u64 remaining = done < size ? size - done : 0;
	u64 eta = bytes_per_second > 0 ? (u64)(remaining / bytes_per_second) : 0;
	printf("\r\033[2K%lu/%lu MiB  %.2f MiB/s", (unsigned long)(done / 1048576),
		(unsigned long)(size / 1048576), bytes_per_second / 1048576.0);
	if (bytes_per_second > 0)
		printf("  ETA %llu:%02llu", (unsigned long long)(eta / 60), (unsigned long long)(eta % 60));
	// Download progress does not need to wait for a display frame.
	gfxFlushBuffers();
	gfxSwapBuffers();
}

// Streams to "<dest>.part" and renames on success so a failed download never clobbers a good file.
static bool download(const char *remote, const char *dest) {
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
	download_progress(0, size, started, started);
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
			download_progress(done, size, started, now);
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
	if (ok) download_progress(done, size, started, svcGetSystemTick());
	if (ok) {
		remove(dest);
		if (rename(part, dest) != 0) { printf("\nRename to %s failed\n", dest); ok = false; }
	}
	if (!ok) remove(part);
	return ok;
}

static void download_flow(const char *path, const char *name, long long size) {
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
		printf("\nDone!");
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
		printf("\nA:open B:back Y:filter X:server\nR:refresh START:quit");

		u32 k = wait_key();
		if (n && (k & KEY_DOWN)) *cur = (*cur + 1) % n;
		else if (n && (k & KEY_UP)) *cur = (*cur + n - 1) % n;
		else if (n && (k & KEY_RIGHT)) *cur = *cur + page < n ? *cur + page : n - 1;
		else if (n && (k & KEY_LEFT)) *cur = *cur > page ? *cur - page : 0;
		else if (k & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_R | KEY_START)) return k;
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
