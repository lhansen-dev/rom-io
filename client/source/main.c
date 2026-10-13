// rom-io 3DS client: browse the rom-io server over HTTP and download ROMs to the SD card.
#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CFG_DIR "sdmc:/3ds/rom-io"
#define CFG_FILE CFG_DIR "/config.txt"
#define ROWS 26 // listing rows that fit on the top screen under the title

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

// Streams to "<dest>.part" and renames on success so a failed download never clobbers a good file.
static bool download(const char *remote, const char *dest) {
	static u8 buf[0x10000];
	char part[600];
	snprintf(part, sizeof part, "%s.part", dest);
	httpcContext c;
	u32 size, done = 0, got;
	if (!http_open(&c, remote, &size)) return false;
	FILE *f = fopen(part, "wb");
	if (!f) { printf("Can't create %s\n", part); httpcCloseContext(&c); return false; }

	bool ok = true;
	Result r;
	do {
		got = 0;
		r = httpcDownloadData(&c, buf, sizeof buf, &got);
		if (got && fwrite(buf, 1, got, f) != got) { printf("\nWrite failed (SD full?)\n"); ok = false; break; }
		done += got;
		printf("\r%lu / %lu KB   ", (unsigned long)done / 1024, (unsigned long)size / 1024);
		present();
		hidScanInput();
		if (hidKeysDown() & KEY_B) { printf("\nCancelled\n"); ok = false; break; }
	} while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
	httpcCloseContext(&c);
	if (ok && (R_FAILED(r) || done != size)) { printf("\nDownload error %08lX\n", (unsigned long)r); ok = false; }
	if (fclose(f) != 0 && ok) { printf("\nWrite failed\n"); ok = false; }
	if (ok) {
		remove(dest);
		if (rename(part, dest) != 0) { printf("\nRename to %s failed\n", dest); ok = false; }
	}
	if (!ok) remove(part);
	return ok;
}

static void download_flow(const char *path, const char *name) {
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
		printf("%s already exists.\n\nA: overwrite   B: cancel\n", dest);
		if (!(wait_key() & KEY_A)) return;
	}
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

// Draws the listing and handles scrolling; returns the key (A/B/X/START) that left the menu.
static u32 menu(const char *path, Entry *e, int n, int *cur) {
	for (;;) {
		consoleClear();
		printf("%.49s\n%.49s\n", server, path);
		int top = *cur - ROWS / 2;
		if (top > n - ROWS) top = n - ROWS;
		if (top < 0) top = 0;
		for (int i = top; i < n && i < top + ROWS; i++) {
			char sz[16];
			fmt_size(sz, e[i].size);
			printf("%c %-38.38s %8s\n", i == *cur ? '>' : ' ', e[i].name, sz);
		}
		if (!n) printf("  (empty)\n");
		printf("\nA:open B:back X:server START:quit");

		u32 k = wait_key();
		if (n && (k & KEY_DOWN)) *cur = (*cur + 1) % n;
		else if (n && (k & KEY_UP)) *cur = (*cur + n - 1) % n;
		else if (n && (k & KEY_RIGHT)) *cur = *cur + ROWS < n ? *cur + ROWS : n - 1;
		else if (n && (k & KEY_LEFT)) *cur = *cur > ROWS ? *cur - ROWS : 0;
		else if (k & (KEY_A | KEY_B | KEY_X | KEY_START)) return k;
	}
}

int main(void) {
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	httpcInit(0);
	load_config();
	if (!server[0]) {
		if (!keyboard("Server IP:port, e.g. 192.168.1.10:8765", server, sizeof server)) goto quit;
		save_config();
	}

	char path[1024] = "/";
	for (;;) {
		consoleClear();
		char *list = fetch_listing(path);
		if (!list) {
			printf("\nCouldn't load %s\n\nA: retry   X: change server   START: quit\n", path);
			u32 k = wait_key();
			if (k & KEY_START) break;
			if ((k & KEY_X) && keyboard("Server IP:port", server, sizeof server)) { save_config(); strcpy(path, "/"); }
			continue;
		}

		int n = 0, max = 1;
		for (char *p = list; *p; p++) max += *p == '\n';
		Entry *e = calloc(max, sizeof *e);
		for (char *line = strtok(list, "\n"); line; line = strtok(NULL, "\n")) {
			char *tab = strchr(line, '\t');
			e[n] = (Entry){ line, -1 };
			if (tab) { *tab = 0; e[n].size = strtoll(tab + 1, NULL, 10); }
			n++;
		}

		int cur = 0;
		bool quit = false, reload = false;
		while (!quit && !reload) {
			u32 k = menu(path, e, n, &cur);
			if (k & KEY_START) quit = true;
			else if (k & KEY_X) {
				if (keyboard("Server IP:port", server, sizeof server)) { save_config(); strcpy(path, "/"); reload = true; }
			} else if (k & KEY_B) {
				if (strcmp(path, "/")) { // drop the last "name/" component
					path[strlen(path) - 1] = 0;
					strrchr(path, '/')[1] = 0;
					reload = true;
				}
			} else if ((k & KEY_A) && n) {
				if (e[cur].size < 0) {
					if (strlen(path) + strlen(e[cur].name) < sizeof path) { strcat(path, e[cur].name); reload = true; }
				} else {
					download_flow(path, e[cur].name);
				}
			}
		}
		free(e);
		free(list);
		if (quit) break;
	}

quit:
	httpcExit();
	gfxExit();
	return 0;
}
