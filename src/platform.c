#include "platform.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>

void platform_rwlock_init(PlatformRwLock *lock) { InitializeSRWLock(lock); }
void platform_rwlock_destroy(PlatformRwLock *lock) { (void)lock; }
void platform_read_lock(PlatformRwLock *lock) { AcquireSRWLockShared(lock); }
void platform_read_unlock(PlatformRwLock *lock) { ReleaseSRWLockShared(lock); }
void platform_write_lock(PlatformRwLock *lock) { AcquireSRWLockExclusive(lock); }
void platform_write_unlock(PlatformRwLock *lock) { ReleaseSRWLockExclusive(lock); }
void platform_mutex_init(PlatformMutex *lock) { InitializeCriticalSection(lock); }
void platform_mutex_destroy(PlatformMutex *lock) { DeleteCriticalSection(lock); }
void platform_mutex_lock(PlatformMutex *lock) { EnterCriticalSection(lock); }
void platform_mutex_unlock(PlatformMutex *lock) { LeaveCriticalSection(lock); }
bool platform_create_directory(const char *path) {
    if (_mkdir(path) == 0) return true;
    struct _stat64 info;
    return errno == EEXIST && _stat64(path, &info) == 0 && (info.st_mode & _S_IFDIR);
}
bool platform_seek(FILE *file, uint64_t offset) {
    return offset <= INT64_MAX && _fseeki64(file, (int64_t)offset, SEEK_SET) == 0;
}
bool platform_file_size(FILE *file, uint64_t *size) {
    struct _stat64 info;
    if (_fstat64(_fileno(file), &info) != 0) return false;
    *size = (uint64_t)info.st_size;
    return true;
}
bool platform_truncate(FILE *file, uint64_t size) {
    return size <= INT64_MAX && _chsize_s(_fileno(file), size) == 0;
}
bool platform_sync(FILE *file) { return _commit(_fileno(file)) == 0; }
bool platform_lock_file(const char *path, PlatformFileLock *lock) {
    *lock = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return *lock != INVALID_HANDLE_VALUE;
}
void platform_unlock_file(PlatformFileLock lock) { CloseHandle(lock); }
double platform_now(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}
#else
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

void platform_rwlock_init(PlatformRwLock *lock) { pthread_rwlock_init(lock, NULL); }
void platform_rwlock_destroy(PlatformRwLock *lock) { pthread_rwlock_destroy(lock); }
void platform_read_lock(PlatformRwLock *lock) { pthread_rwlock_rdlock(lock); }
void platform_read_unlock(PlatformRwLock *lock) { pthread_rwlock_unlock(lock); }
void platform_write_lock(PlatformRwLock *lock) { pthread_rwlock_wrlock(lock); }
void platform_write_unlock(PlatformRwLock *lock) { pthread_rwlock_unlock(lock); }
void platform_mutex_init(PlatformMutex *lock) { pthread_mutex_init(lock, NULL); }
void platform_mutex_destroy(PlatformMutex *lock) { pthread_mutex_destroy(lock); }
void platform_mutex_lock(PlatformMutex *lock) { pthread_mutex_lock(lock); }
void platform_mutex_unlock(PlatformMutex *lock) { pthread_mutex_unlock(lock); }
bool platform_create_directory(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
bool platform_seek(FILE *file, uint64_t offset) {
    return offset <= INT64_MAX && fseeko(file, (off_t)offset, SEEK_SET) == 0;
}
bool platform_file_size(FILE *file, uint64_t *size) {
    struct stat info;
    if (fstat(fileno(file), &info) != 0) return false;
    *size = (uint64_t)info.st_size;
    return true;
}
bool platform_truncate(FILE *file, uint64_t size) {
    return size <= INT64_MAX && ftruncate(fileno(file), (off_t)size) == 0;
}
bool platform_sync(FILE *file) { return fsync(fileno(file)) == 0; }
bool platform_lock_file(const char *path, PlatformFileLock *lock) {
    *lock = open(path, O_CREAT | O_RDWR, 0644);
    if (*lock < 0) return false;
    if (flock(*lock, LOCK_EX | LOCK_NB) == 0) return true;
    close(*lock);
    return false;
}
void platform_unlock_file(PlatformFileLock lock) { close(lock); }
double platform_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
#endif
