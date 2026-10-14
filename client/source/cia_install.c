#include "cia_install.h"
#include <stdlib.h>
#include <string.h>

#define INSTALL_CHUNK (256 * 1024)

bool cia_install(const char *path, CiaConfirm confirm, CiaProgress progress,
	const char **error, Result *result) {
	Handle source = 0, target = 0;
	bool am_ready = false, installing = false, success = false;
	u8 *buffer = NULL;
	u64 size = 0, required = 0, done = 0;
	AM_TitleInfo info;
	FS_ArchiveResource space;
	*result = 0;
	*error = NULL;
	if (strncmp(path, "sdmc:/", 6) != 0) {
		*error = "Installation needs an absolute sdmc:/ path.";
		goto cleanup;
	}
	*error = "Can't access installer. Run under Luma3DS CFW.";
	if (R_FAILED(*result = amInit())) goto cleanup;
	am_ready = true;
	*error = "Can't open downloaded CIA.";
	if (R_FAILED(*result = FSUSER_OpenFileDirectly(&source, ARCHIVE_SDMC,
		fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path + 5), FS_OPEN_READ, 0))) goto cleanup;
	*error = "Invalid or unreadable CIA.";
	if (R_FAILED(*result = FSFILE_GetSize(source, &size)) || !size) goto cleanup;
	if (R_FAILED(*result = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, source))) goto cleanup;
	// Restrict installation to SD applications, demos, updates, and DLC.
	u32 category = (u32)(info.titleID >> 32);
	if (category != 0x00040000 && category != 0x00040002 &&
		category != 0x0004000e && category != 0x0004008c) {
		*error = "Only SD applications, demos, updates and DLC can be installed.";
		goto cleanup;
	}
	*error = "Can't check installation space.";
	if (R_FAILED(*result = AM_GetCiaRequiredSpace(&required, MEDIATYPE_SD, source))) goto cleanup;
	if (R_FAILED(*result = FSUSER_GetSdmcArchiveResource(&space))) goto cleanup;
	if (required > (u64)space.clusterSize * space.freeClusters) {
		*error = "Not enough SD space for installed contents.";
		goto cleanup;
	}
	*error = "Installation cancelled.";
	if (!confirm(&info, required) || !progress(0, size)) goto cleanup;
	buffer = malloc(INSTALL_CHUNK);
	if (!buffer) { *error = "Not enough memory for installation."; goto cleanup; }
	*error = "Can't start CIA installation.";
	if (R_FAILED(*result = AM_StartCiaInstall(MEDIATYPE_SD, &target))) goto cleanup;
	installing = true;
	while (done < size) {
		u32 got = 0, written = 0;
		u32 count = size - done < INSTALL_CHUNK ? (u32)(size - done) : INSTALL_CHUNK;
		*error = "Can't read downloaded CIA.";
		if (R_FAILED(*result = FSFILE_Read(source, &got, done, buffer, count)) || got != count) goto cleanup;
		*error = "CIA installation write failed.";
		if (R_FAILED(*result = FSFILE_Write(target, &written, done, buffer, got, 0)) || written != got) goto cleanup;
		done += written;
		*error = "Installation cancelled.";
		if (!progress(done, size)) goto cleanup;
	}
	*error = "Can't finish CIA installation.";
	installing = false; // Finish moves the handle, including on a service error.
	if (R_FAILED(*result = AM_FinishCiaInstall(target))) goto cleanup;
	success = true;
	*error = NULL;
cleanup:
	if (installing) {
		Result cancelled = AM_CancelCIAInstall(target);
		if (R_FAILED(cancelled)) {
			*error = "Installation failed; cleanup also failed.";
			*result = cancelled;
		}
	}
	free(buffer);
	if (source) FSFILE_Close(source);
	if (am_ready) amExit();
	return success;
}
