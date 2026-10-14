#pragma once
#include <3ds.h>

#define MAX_DOWNLOAD_STREAMS 4

// Split into contiguous ranges with no gaps or overlaps, including odd sizes.
// Multiply in 64 bits so partitioning large files cannot overflow.
static inline void download_range(u32 total, unsigned index, unsigned count, u32 *start, u32 *length) {
	u64 first = (u64)total * index / count;
	u64 next = (u64)total * (index + 1) / count;
	*start = first;
	*length = next - first;
}
