// Host test substitute for the libctru calls used by download_stream.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int32_t Result;
#define U64_MAX UINT64_MAX
#define HTTPC_RESULTCODE_DOWNLOADPENDING 0xd840a02b
typedef pthread_mutex_t LightLock;
typedef pthread_cond_t CondVar;
typedef struct HostThread *Thread;
typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	u64 length, offset, fail_at, base;
	unsigned calls;
	bool blocked, cancelled;
} httpcContext;

void LightLock_Init(LightLock *lock);
void LightLock_Lock(LightLock *lock);
void LightLock_Unlock(LightLock *lock);
void CondVar_Init(CondVar *cv);
void CondVar_Wait(CondVar *cv, LightLock *lock);
int CondVar_WaitTimeout(CondVar *cv, LightLock *lock, s64 timeout);
void CondVar_Broadcast(CondVar *cv);
Thread threadCreate(void (*entry)(void *), void *arg, size_t stack, int priority, int core, bool detached);
Result threadJoin(Thread thread, u64 timeout);
void threadFree(Thread thread);
Result httpcDownloadData(httpcContext *context, u8 *buffer, u32 size, u32 *got);
Result httpcCancelConnection(httpcContext *context);
