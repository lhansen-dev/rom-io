#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t Result;
typedef u32 Handle;
typedef struct { u64 titleID, size; uint16_t version; u32 titleType; } AM_TitleInfo;
typedef struct { u32 sectorSize, clusterSize, totalClusters, freeClusters; } FS_ArchiveResource;
typedef struct { int type; const void *data; } FS_Path;
#define R_FAILED(r) ((r) < 0)
#define MEDIATYPE_SD 1
#define ARCHIVE_SDMC 9
#define PATH_EMPTY 1
#define PATH_UTF16 4
#define FS_OPEN_READ 1
static inline FS_Path fsMakePath(int type, const void *path) { return (FS_Path){type, path}; }
ssize_t utf8_to_utf16(u16 *out, const u8 *in, size_t len);
Result amInit(void);
void amExit(void);
Result FSUSER_OpenFileDirectly(Handle *, int, FS_Path, FS_Path, u32, u32);
Result FSFILE_GetSize(Handle, u64 *);
Result AM_GetCiaFileInfo(int, AM_TitleInfo *, Handle);
Result AM_GetCiaRequiredSpace(u64 *, int, Handle);
Result FSUSER_GetSdmcArchiveResource(FS_ArchiveResource *);
Result AM_StartCiaInstall(int, Handle *);
Result FSFILE_Read(Handle, u32 *, u64, void *, u32);
Result FSFILE_Write(Handle, u32 *, u64, const void *, u32, u32);
Result AM_FinishCiaInstall(Handle);
Result AM_CancelCIAInstall(Handle);
Result FSFILE_Close(Handle);
