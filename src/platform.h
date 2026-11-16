#ifndef MINI_PLATFORM_H
#define MINI_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK PlatformRwLock;
typedef CRITICAL_SECTION PlatformMutex;
typedef HANDLE PlatformFileLock;
#else
#include <pthread.h>
typedef pthread_rwlock_t PlatformRwLock;
typedef pthread_mutex_t PlatformMutex;
typedef int PlatformFileLock;
#endif

void platform_rwlock_init(PlatformRwLock *lock);
void platform_rwlock_destroy(PlatformRwLock *lock);
void platform_read_lock(PlatformRwLock *lock);
void platform_read_unlock(PlatformRwLock *lock);
void platform_write_lock(PlatformRwLock *lock);
void platform_write_unlock(PlatformRwLock *lock);
void platform_mutex_init(PlatformMutex *lock);
void platform_mutex_destroy(PlatformMutex *lock);
void platform_mutex_lock(PlatformMutex *lock);
void platform_mutex_unlock(PlatformMutex *lock);
bool platform_create_directory(const char *path);
bool platform_seek(FILE *file, uint64_t offset);
bool platform_file_size(FILE *file, uint64_t *size);
bool platform_truncate(FILE *file, uint64_t size);
bool platform_sync(FILE *file);
bool platform_lock_file(const char *path, PlatformFileLock *lock);
void platform_unlock_file(PlatformFileLock lock);
double platform_now(void);

#endif
