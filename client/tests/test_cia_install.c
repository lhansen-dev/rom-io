#include "cia_install.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fail, starts, finishes, cancels, closes, exits;
static bool approve, stop;
static u64 title, free_bytes, written_total;
static bool short_read, short_write;
enum { INIT = 1, OPEN, SIZE, INFO, SPACE, RESOURCE, START, READ, WRITE, FINISH, CANCEL };
static Result status(int operation) { return fail == operation ? -operation : 0; }
Result amInit(void) { return status(INIT); }
void amExit(void) { exits++; }
Result FSUSER_OpenFileDirectly(Handle *out, int archive, FS_Path root, FS_Path file, u32 flags, u32 attributes) {
	assert(archive == ARCHIVE_SDMC && flags == FS_OPEN_READ && !attributes);
	assert(!strcmp(root.data, "") && !strcmp(file.data, "/cias/test.cia"));
	if (fail != OPEN) *out = 1;
	return status(OPEN);
}
Result FSFILE_GetSize(Handle file, u64 *size) { assert(file == 1); *size = 600000; return status(SIZE); }
Result AM_GetCiaFileInfo(int media, AM_TitleInfo *info, Handle file) {
	assert(media == MEDIATYPE_SD && file == 1); *info = (AM_TitleInfo){.titleID = title}; return status(INFO);
}
Result AM_GetCiaRequiredSpace(u64 *space, int media, Handle file) {
	assert(media == MEDIATYPE_SD && file == 1); *space = 800000; return status(SPACE);
}
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource *space) {
	*space = (FS_ArchiveResource){.clusterSize = 1, .freeClusters = free_bytes}; return status(RESOURCE);
}
Result AM_StartCiaInstall(int media, Handle *out) {
	assert(media == MEDIATYPE_SD); starts++; *out = 2; return status(START);
}
Result FSFILE_Read(Handle file, u32 *got, u64 offset, void *buffer, u32 count) {
	assert(file == 1 && offset == written_total); memset(buffer, 0x42, count);
	*got = short_read ? count - 1 : count; return status(READ);
}
Result FSFILE_Write(Handle file, u32 *written, u64 offset, const void *buffer, u32 count, u32 flags) {
	assert(file == 2 && offset == written_total && !flags);
	const u8 *bytes = buffer;
	for (u32 i = 0; i < count; i++) assert(bytes[i] == 0x42);
	*written = short_write ? count - 1 : count; written_total += *written; return status(WRITE);
}
Result AM_FinishCiaInstall(Handle file) { assert(file == 2); finishes++; return status(FINISH); }
Result AM_CancelCIAInstall(Handle file) { assert(file == 2); cancels++; return status(CANCEL); }
Result FSFILE_Close(Handle file) { assert(file == 1); closes++; return 0; }
static bool confirm(const AM_TitleInfo *info, u64 required) {
	assert(info->titleID == title && required == 800000); return approve;
}
static bool progress(u64 done, u64 total) { assert(total == 600000 && done == written_total); return !(stop && done); }
static void reset(void) {
	fail = starts = finishes = cancels = closes = exits = 0;
	approve = true; stop = short_read = short_write = false;
	title = 0x0004000001234500ULL; free_bytes = 1000000; written_total = 0;
}
static bool run(void) {
	const char *error; Result result;
	bool ok = cia_install("sdmc:/cias/test.cia", confirm, progress, &error, &result);
	assert(ok ? !error : error != NULL);
	assert(exits == (fail == INIT ? 0 : 1));
	assert(closes == (fail == INIT || fail == OPEN ? 0 : 1));
	return ok;
}
int main(void) {
	reset(); assert(run()); assert(written_total == 600000 && finishes == 1 && !cancels);
	for (int operation = INIT; operation <= FINISH; operation++) {
		reset(); fail = operation; assert(!run());
		assert(cancels == (operation == READ || operation == WRITE));
		assert(finishes == (operation == FINISH));
	}
	reset(); approve = false; assert(!run()); assert(!starts);
	reset(); stop = true; assert(!run()); assert(cancels == 1 && !finishes);
	reset(); short_read = true; assert(!run()); assert(cancels == 1 && !written_total);
	reset(); short_write = true; assert(!run()); assert(cancels == 1 && !finishes);
	reset(); free_bytes = 799999; assert(!run()); assert(!starts);
	reset(); title = 0x0004013800000002ULL; assert(!run()); assert(!starts);
	reset(); title = 0x0004800400000002ULL; assert(!run()); assert(!starts);
	reset(); fail = CANCEL; stop = true; assert(!run()); assert(cancels == 1);
	reset(); const char *error; Result result;
	assert(!cia_install("roms/test.cia", confirm, progress, &error, &result)); assert(!exits && !starts);
	puts("CIA installation tests passed");
}
