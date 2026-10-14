#pragma once
#include <3ds.h>

typedef bool (*CiaConfirm)(const AM_TitleInfo *info, u64 required_space);
typedef bool (*CiaProgress)(u64 done, u64 total);

// Callbacks return false to cancel. The downloaded file is always retained.
bool cia_install(const char *path, CiaConfirm confirm, CiaProgress progress,
	const char **error, Result *result);
