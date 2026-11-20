// SQLite's HOS backend uses native file handles and offsets. The Unix VFS
// assumes POSIX inode identities, permissions and locking that SD/fsdev lacks.
//
// Aurora keeps its pipeline cache (and the shipped pipeline seed) in SQLite databases. This file is a
// complete SQLite VFS named "hos" built on libnx's fs service calls (fsFsOpenFile, fsFileRead, ...),
// registered as the default VFS by sqlite3_os_init below (SQLite is built with SQLITE_OS_OTHER, so it calls
// that instead of setting up its Unix VFS). sdmc: paths are translated to the native file system with
// fsdevTranslatePath.
#include <sqlite3.h>
#include <switch.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <new>
#include <strings.h>
#include <unistd.h>

namespace {
// Horizon fs result codes (module 2): path does not exist, path already exists. lastResult is reported by
// SQLite's xGetLastError.
constexpr Result PathNotFound = 0x202;
constexpr Result PathExists = 0x402;
thread_local Result lastResult;

// HOS rejects a second native open while a writable file handle exists. Keep
// one handle per path in this process; SQLite locks still belong to individual
// connections. Serialize native operations and retain the handle until the last
// connection closes, regardless of which connection opened it first.
//
// Every access to a path goes through this one handle, so the file length and the
// connection that holds the lock are tracked here instead of asking the SD card:
// a native size query per page read, and a directory create/delete pair per SQLite
// lock, were a measurable share of every cache lookup.
struct HosFile;
constexpr u64 BlockSize = 32 * 1024;
constexpr int BlockCount = 4;
// One cached 32 KiB block of a file (BlockCount of them per file, least recently used evicted).
struct Block {
    u64 base = 0;
    u32 length = 0; // 0 = empty
    u64 used = 0;
    char* data = nullptr;
};
// The one native handle per path, shared by every connection that has that path open.
struct SharedFile {
    FsFile handle;
    FsFileSystem* fs;
    u32 mode;
    unsigned references;
    bool deleteOnClose;
    char path[FS_MAX_PATH];
    SharedFile* next;
    s64 size;
    HosFile* lockOwner;
    // Small read-ahead cache: SQLite fetches overflow chains one 4 KiB page at a
    // time, which is one SD round trip per page. Any overlapping write drops it.
    Block blocks[BlockCount];
    u64 tick;
    std::chrono::steady_clock::time_point statStart;
    u64 reads, blockHits, fsReads, fsBytes, fsNs;
};
std::mutex handleMutex;
SharedFile* handles = nullptr;
SharedFile* findHandle(FsFileSystem* fs, const char* path) {
    for (auto* shared = handles; shared; shared = shared->next)
        if (shared->fs == fs && strcasecmp(shared->path, path) == 0)
            return shared;
    return nullptr;
}

// SQLite's per-connection file object; base must be first (SQLite allocates szOsFile bytes and treats the
// start as a sqlite3_file). lock is this connection's SQLite lock level.
struct HosFile {
    sqlite3_file base;
    SharedFile* shared;
    FsFileSystem* fs;
    int lock;
    bool readOnly;
    char path[FS_MAX_PATH];
};
HosFile* file(sqlite3_file* value) { return reinterpret_cast<HosFile*>(value); }
// Records the native result for xGetLastError, logs it, and returns the SQLite error code.
int error(Result result, int code, const char* operation, const char* path) {
    lastResult = result;
    std::fprintf(stderr, "[sqlite-hos] %s failed: result=%08x path=%s\n", operation, result, path);
    return code;
}
// Lowering to NONE releases the file-wide lock ownership (see lock()).
int unlock(sqlite3_file* base, int level) {
    auto* f = file(base);
    if (level == SQLITE_LOCK_NONE && f->lock != SQLITE_LOCK_NONE) {
        std::lock_guard guard(handleMutex);
        if (f->shared->lockOwner == f)
            f->shared->lockOwner = nullptr;
    }
    f->lock = level;
    return SQLITE_OK;
}
// Drops every cached block that overlaps [offset, offset + amount).
void invalidateBlocks(SharedFile* shared, u64 offset, u64 amount) {
    for (auto& block : shared->blocks)
        if (block.length && offset < block.base + BlockSize && block.base < offset + amount)
            block.length = 0;
}
// Closes one connection's view; the native handle closes with the last one.
int close(sqlite3_file* base) {
    auto* f = file(base);
    const int result = unlock(base, SQLITE_LOCK_NONE);
    std::lock_guard guard(handleMutex);
    auto* shared = f->shared;
    if (--shared->references == 0) {
        // Horizon commits written data on flush or clean close only. fsFileClose
        // alone has been observed to drop the tail of a grown database, leaving a
        // header that claims more pages than the file holds.
        if (shared->mode & FsOpenMode_Write)
            fsFileFlush(&shared->handle);
        fsFileClose(&shared->handle);
        if (shared->deleteOnClose)
            fsFsDeleteFile(shared->fs, shared->path);
        auto** entry = &handles;
        while (*entry != shared)
            entry = &(*entry)->next;
        *entry = shared->next;
        for (auto& block : shared->blocks)
            std::free(block.data);
        delete shared;
    }
    f->shared = nullptr;
    f->base.pMethods = nullptr;
    return result;
}
// Reads straight from the card into `buffer`. Caller holds handleMutex.
Result readRaw(SharedFile* shared, u64 offset, void* buffer, u64 requested, u64& count) {
    const auto start = std::chrono::steady_clock::now();
    count = 0;
    Result rc = fsFileRead(&shared->handle, offset, buffer, requested, FsReadOption_None, &count);
    if ((rc & 0x3fffff) == 0xd401) {
        // Like fsdev, bounce buffers from memory that HOS cannot map for IPC.
        char bounce[4096];
        count = 0;
        while (count < requested) {
            const u64 chunk =
                requested - count < sizeof(bounce) ? requested - count : sizeof(bounce);
            u64 received = 0;
            rc = fsFileRead(&shared->handle, offset + count, bounce, chunk, FsReadOption_None,
                            &received);
            if (R_FAILED(rc))
                break;
            if (received > chunk) {
                count = 0;
                rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
                break;
            }
            std::memcpy(static_cast<char*>(buffer) + count, bounce, received);
            count += received;
            if (received < chunk)
                break;
        }
    }
    ++shared->fsReads;
    shared->fsBytes += count;
    shared->fsNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    return rc;
}
// The cached block starting at `base`, or null. Marks it recently used.
Block* findBlock(SharedFile* shared, u64 base) {
    for (auto& block : shared->blocks) {
        if (block.length && block.base == base) {
            block.used = ++shared->tick;
            return &block;
        }
    }
    return nullptr;
}
// Fills a block from the card, evicting the least recently used. Null on any
// failure; the caller then reads directly.
Block* loadBlock(SharedFile* shared, u64 base) {
    Block* victim = nullptr;
    for (auto& block : shared->blocks) {
        if (!block.length) {
            victim = &block;
            break;
        }
        if (!victim || block.used < victim->used)
            victim = &block;
    }
    if (!victim->data) {
        victim->data = static_cast<char*>(std::aligned_alloc(0x1000, BlockSize));
        if (!victim->data)
            return nullptr;
    }
    const u64 remaining = static_cast<u64>(shared->size) - base;
    const u64 length = remaining < BlockSize ? remaining : BlockSize;
    u64 count = 0;
    victim->length = 0;
    if (R_FAILED(readRaw(shared, base, victim->data, length, count)) || count != length)
        return nullptr;
    victim->base = base;
    victim->length = static_cast<u32>(length);
    victim->used = ++shared->tick;
    return victim;
}
// Logs a SQLVFS line (reads, cache hits, native reads, KiB and time spent) at most every 2 s per file.
void reportStats(SharedFile* shared, std::chrono::steady_clock::time_point now) {
    if (shared->reads) {
        const char* name = std::strrchr(shared->path, '/');
        std::fprintf(stderr,
                     "[sqlite-hos] SQLVFS %s reads %llu block_hits %llu fs_reads %llu fs_kb %llu "
                     "fs_ms %.1f over %lldms\n",
                     name ? name + 1 : shared->path, static_cast<unsigned long long>(shared->reads),
                     static_cast<unsigned long long>(shared->blockHits),
                     static_cast<unsigned long long>(shared->fsReads),
                     static_cast<unsigned long long>(shared->fsBytes / 1024), shared->fsNs / 1e6,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                now - shared->statStart)
                                                .count()));
    }
    shared->statStart = now;
    shared->reads = shared->blockHits = shared->fsReads = shared->fsBytes = shared->fsNs = 0;
}
// xRead: served from the block cache, loading blocks as needed. Reads past the end are zero-filled and
// reported as a short read, which is what SQLite expects.
int read(sqlite3_file* base, void* buffer, int amount, sqlite3_int64 offset) {
    auto* f = file(base);
    std::lock_guard guard(handleMutex);
    auto* shared = f->shared;
    const auto now = std::chrono::steady_clock::now();
    if (now - shared->statStart >= std::chrono::seconds(2))
        reportStats(shared, now);
    ++shared->reads;
    const s64 length = shared->size;
    const u64 available = offset < length ? static_cast<u64>(length - offset) : 0;
    const u64 requested =
        available < static_cast<u64>(amount) ? available : static_cast<u64>(amount);
    u64 count = 0;
    Result rc = 0;
    while (count < requested) {
        const u64 position = static_cast<u64>(offset) + count;
        const u64 blockBase = position / BlockSize * BlockSize;
        Block* block = findBlock(shared, blockBase);
        if (block)
            ++shared->blockHits;
        else
            block = loadBlock(shared, blockBase);
        if (!block) {
            // Could not stage a block: read the remainder directly.
            u64 direct = 0;
            rc = readRaw(shared, position, static_cast<char*>(buffer) + count, requested - count,
                         direct);
            count += direct;
            break;
        }
        const u64 within = position - block->base;
        const u64 take = requested - count < block->length - within ? requested - count
                                                                    : block->length - within;
        std::memcpy(static_cast<char*>(buffer) + count, block->data + within, take);
        count += take;
    }
    if (R_FAILED(rc))
        return error(rc, SQLITE_IOERR_READ, "read", f->path);
    if (count < static_cast<u64>(amount)) {
        std::memset(static_cast<char*>(buffer) + count, 0, amount - count);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}
// xWrite: writes through to the card (no write cache) after dropping overlapping cached blocks; uses a
// stack bounce buffer when the source memory cannot be mapped for IPC (error 0xd401).
int write(sqlite3_file* base, const void* buffer, int amount, sqlite3_int64 offset) {
    auto* f = file(base);
    if (f->readOnly)
        return SQLITE_READONLY;
    std::lock_guard guard(handleMutex);
    invalidateBlocks(f->shared, offset, amount);
    Result rc = fsFileWrite(&f->shared->handle, offset, buffer, amount, FsWriteOption_None);
    if ((rc & 0x3fffff) == 0xd401) {
        char bounce[4096];
        int written = 0;
        while (written < amount) {
            const int chunk = amount - written < static_cast<int>(sizeof(bounce)) ? amount - written
                                                                                  : sizeof(bounce);
            std::memcpy(bounce, static_cast<const char*>(buffer) + written, chunk);
            rc = fsFileWrite(&f->shared->handle, offset + written, bounce, chunk,
                             FsWriteOption_None);
            if (R_FAILED(rc))
                break;
            written += chunk;
        }
    }
    if (R_FAILED(rc))
        return error(rc, SQLITE_IOERR_WRITE, "write", f->path);
    if (offset + amount > f->shared->size)
        f->shared->size = offset + amount;
    return SQLITE_OK;
}
// xTruncate: sets the native file size and drops cached blocks past it.
int truncate(sqlite3_file* base, sqlite3_int64 size) {
    auto* f = file(base);
    if (f->readOnly)
        return SQLITE_READONLY;
    std::lock_guard guard(handleMutex);
    invalidateBlocks(f->shared, size, UINT64_MAX - size);
    const Result rc = fsFileSetSize(&f->shared->handle, size);
    if (R_FAILED(rc))
        return error(rc, SQLITE_IOERR_TRUNCATE, "truncate", f->path);
    f->shared->size = size;
    return SQLITE_OK;
}
// xSync: fsFileFlush commits written data to the card.
int sync(sqlite3_file* base, int) {
    auto* f = file(base);
    std::lock_guard guard(handleMutex);
    const Result rc = fsFileFlush(&f->shared->handle);
    return R_SUCCEEDED(rc) ? SQLITE_OK : error(rc, SQLITE_IOERR_FSYNC, "flush", f->path);
}
// xFileSize: the tracked size, no native query.
int size(sqlite3_file* base, sqlite3_int64* output) {
    auto* f = file(base);
    std::lock_guard guard(handleMutex);
    *output = f->shared->size;
    return SQLITE_OK;
}
// xLock: SQLite's lock levels are kept per connection; only the step out of NONE needs exclusion
// between connections, and it is granted to one connection at a time (others get SQLITE_BUSY and retry).
int lock(sqlite3_file* base, int level) {
    auto* f = file(base);
    if (level <= f->lock)
        return SQLITE_OK;
    if (f->lock == SQLITE_LOCK_NONE) {
        // Coarse exclusion between connections to one file: the first to take any
        // lock owns it until it drops back to none. The shared handle already
        // limits the file to this process, so nothing needs to reach the card.
        std::lock_guard guard(handleMutex);
        if (f->shared->lockOwner && f->shared->lockOwner != f)
            return SQLITE_BUSY;
        f->shared->lockOwner = f;
    }
    f->lock = level;
    return SQLITE_OK;
}
// xCheckReservedLock: whether any connection holds a lock on the file.
int reserved(sqlite3_file* base, int* output) {
    auto* f = file(base);
    if (f->lock != SQLITE_LOCK_NONE) {
        *output = f->lock >= SQLITE_LOCK_RESERVED;
        return SQLITE_OK;
    }
    std::lock_guard guard(handleMutex);
    *output = f->shared->lockOwner != nullptr;
    return SQLITE_OK;
}
// xFileControl: answers the few queries SQLite needs; everything else is "not supported".
int control(sqlite3_file* base, int operation, void* argument) {
    if (operation == SQLITE_FCNTL_LOCKSTATE) {
        *static_cast<int*>(argument) = file(base)->lock;
        return SQLITE_OK;
    }
    if (operation == SQLITE_FCNTL_HAS_MOVED) {
        *static_cast<int*>(argument) = 0; // private cache paths stay fixed while open
        return SQLITE_OK;
    }
    if (operation == SQLITE_FCNTL_SIZE_HINT) {
        // Grow the file in one explicit step rather than letting each page write
        // extend it implicitly: fewer metadata updates, and the final size is
        // committed by the next flush even if a page write is later lost.
        auto* f = file(base);
        if (f->readOnly)
            return SQLITE_OK;
        const sqlite3_int64 hint = *static_cast<sqlite3_int64*>(argument);
        std::lock_guard guard(handleMutex);
        if (hint > f->shared->size && R_SUCCEEDED(fsFileSetSize(&f->shared->handle, hint)))
            f->shared->size = hint;
        return SQLITE_OK;
    }
    return SQLITE_NOTFOUND;
}
// SD card sector size used by SQLite to size its writes.
int sector(sqlite3_file*) { return 4096; }
int characteristics(sqlite3_file*) { return 0; } // do not promise atomic writes
// Method table, version 1 (no WAL shared memory, no memory-mapped I/O).
const sqlite3_io_methods methods = {
    1,       close,    read,    write,  truncate,        sync,    size,    lock,
    unlock,  reserved, control, sector, characteristics, nullptr, nullptr, nullptr,
    nullptr, nullptr,  nullptr};

// xFullPathname: paths with a device ("sdmc:/") or a leading slash are kept, others are made relative
// to the current directory.
int fullPath(sqlite3_vfs*, const char* input, int capacity, char* output) {
    char cwd[FS_MAX_PATH];
    int length;
    if (std::strstr(input, ":/") || input[0] == '/') {
        length = std::snprintf(output, capacity, "%s", input);
    } else {
        if (!getcwd(cwd, sizeof(cwd)))
            return SQLITE_CANTOPEN;
        length = std::snprintf(output, capacity, "%s/%s", cwd, input);
    }
    return length >= 0 && length < capacity ? SQLITE_OK : SQLITE_CANTOPEN;
}
// xOpen: translates and normalises the path, creates the file if asked, and joins or creates the shared
// handle. A null name means a temporary database: it gets a random name in cache/ (so nothing lands next
// to the NRO) and is deleted when closed.
int open(sqlite3_vfs*, const char* name, sqlite3_file* base, int flags, int* actualFlags) {
    auto* f = file(base);
    std::memset(f, 0, sizeof(*f));
    char temporary[FS_MAX_PATH];
    if (!name) {
        u64 nonce;
        randomGet(&nonce, sizeof(nonce));
        std::snprintf(temporary, sizeof(temporary), "sdmc:/switch/SFA-NX/cache/tmp-%016llx.db",
                      static_cast<unsigned long long>(nonce));
        name = temporary;
        flags |= SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE;
    }
    if (fsdevTranslatePath(name, &f->fs, f->path) != 0)
        return SQLITE_CANTOPEN;
    try {
        const auto normalized = std::filesystem::path(f->path).lexically_normal().generic_string();
        if (normalized.size() >= sizeof(f->path))
            return SQLITE_CANTOPEN;
        std::strcpy(f->path, normalized.c_str());
    } catch (const std::bad_alloc&) {
        return SQLITE_NOMEM;
    }
    std::lock_guard guard(handleMutex);
    auto* shared = findHandle(f->fs, f->path);
    if (shared && (flags & SQLITE_OPEN_EXCLUSIVE))
        return SQLITE_CANTOPEN;
    if (flags & SQLITE_OPEN_CREATE) {
        const Result rc = fsFsCreateFile(f->fs, f->path, 0, 0);
        if (R_FAILED(rc) && (rc != PathExists || (flags & SQLITE_OPEN_EXCLUSIVE)))
            return error(rc, SQLITE_CANTOPEN, "create", f->path);
    }
    if (!shared) {
        shared = new (std::nothrow) SharedFile{};
        if (!shared)
            return SQLITE_NOMEM;
        // Keep main databases shareable if a read-only connection opens first.
        // Connection-level readOnly below still forbids writes through that view.
        u32 mode = FsOpenMode_Read;
        if (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_MAIN_DB))
            mode |= FsOpenMode_Write | FsOpenMode_Append;
        Result rc = fsFsOpenFile(f->fs, f->path, mode, &shared->handle);
        if (R_FAILED(rc) && (mode & FsOpenMode_Write)) {
            mode = FsOpenMode_Read;
            rc = fsFsOpenFile(f->fs, f->path, mode, &shared->handle);
        }
        if (R_FAILED(rc)) {
            delete shared;
            return error(rc, SQLITE_CANTOPEN, "open", f->path);
        }
        s64 length = 0;
        rc = fsFileGetSize(&shared->handle, &length);
        if (R_FAILED(rc)) {
            fsFileClose(&shared->handle);
            delete shared;
            return error(rc, SQLITE_CANTOPEN, "size", f->path);
        }
        shared->size = length;
        shared->statStart = std::chrono::steady_clock::now();
        shared->fs = f->fs;
        shared->mode = mode;
        std::strcpy(shared->path, f->path);
        shared->next = handles;
        handles = shared;
    }
    ++shared->references;
    shared->deleteOnClose |= (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    f->shared = shared;
    f->readOnly = !(flags & SQLITE_OPEN_READWRITE) || !(shared->mode & FsOpenMode_Write);
    if (f->readOnly)
        flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
    f->base.pMethods = &methods;
    if (actualFlags)
        *actualFlags = flags;
    return SQLITE_OK;
}
// xDelete: deleting a file that is already gone counts as success.
int remove(sqlite3_vfs*, const char* name, int) {
    FsFileSystem* fs;
    char path[FS_MAX_PATH];
    if (fsdevTranslatePath(name, &fs, path) != 0)
        return SQLITE_IOERR_DELETE;
    const Result rc = fsFsDeleteFile(fs, path);
    return R_SUCCEEDED(rc) || rc == PathNotFound ? SQLITE_OK
                                                 : error(rc, SQLITE_IOERR_DELETE, "delete", path);
}
// xAccess: does the path exist, and can it be opened read/write. Answers from an open shared handle when
// there is one, because Horizon would refuse a second writable open.
int access(sqlite3_vfs*, const char* name, int flags, int* output) {
    FsFileSystem* fs;
    char path[FS_MAX_PATH];
    *output = 0;
    if (fsdevTranslatePath(name, &fs, path) != 0)
        return SQLITE_IOERR_ACCESS;
    FsDirEntryType type;
    Result rc = fsFsGetEntryType(fs, path, &type);
    if (rc == PathNotFound)
        return SQLITE_OK;
    if (R_FAILED(rc))
        return error(rc, SQLITE_IOERR_ACCESS, "access", path);
    if (flags == SQLITE_ACCESS_EXISTS) {
        *output = 1;
        return SQLITE_OK;
    }
    std::lock_guard guard(handleMutex);
    if (auto* shared = findHandle(fs, path)) {
        *output = flags != SQLITE_ACCESS_READWRITE || (shared->mode & FsOpenMode_Write);
        return SQLITE_OK;
    }
    FsFile handle;
    rc = fsFsOpenFile(fs, path,
                      FsOpenMode_Read | (flags == SQLITE_ACCESS_READWRITE ? FsOpenMode_Write : 0),
                      &handle);
    if (R_SUCCEEDED(rc)) {
        *output = 1;
        fsFileClose(&handle);
    }
    return SQLITE_OK;
}
// xRandomness, xSleep and the clock: libnx random bytes, svcSleepThread, and the wall clock as a Julian
// day number in milliseconds (210866760000000 is the Unix epoch on that scale).
int randomness(sqlite3_vfs*, int amount, char* bytes) {
    randomGet(bytes, amount);
    return amount;
}
int sleep(sqlite3_vfs*, int microseconds) {
    svcSleepThread(static_cast<s64>(microseconds) * 1000);
    return microseconds;
}
int currentTime64(sqlite3_vfs*, sqlite3_int64* output) {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return SQLITE_ERROR;
    *output =
        210866760000000LL + static_cast<sqlite3_int64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
    return SQLITE_OK;
}
int currentTime(sqlite3_vfs* vfs, double* output) {
    sqlite3_int64 value = 0;
    const int result = currentTime64(vfs, &value);
    *output = static_cast<double>(value) / 86400000.0;
    return result;
}
// xGetLastError: the native result of the last failed call on this thread.
int lastError(sqlite3_vfs*, int length, char* output) {
    if (length > 0)
        std::snprintf(output, length, "HOS result %08x", lastResult);
    return static_cast<int>(lastResult);
}
// VFS version 2 (adds xCurrentTimeInt64).
sqlite3_vfs vfs = {
    2,           sizeof(HosFile), FS_MAX_PATH - 1, nullptr, "hos",   nullptr, open,       remove,
    access,      fullPath,        nullptr,         nullptr, nullptr, nullptr, randomness, sleep,
    currentTime, lastError,       currentTime64,   nullptr, nullptr, nullptr};
} // namespace

// Fast exit skips SQLite shutdown. Flush writable handles before process exit.
// Use bounded lock attempts because an abort may originate inside SQLite.
extern "C" void melee_nx_sqlite_flush_all(void) {
    std::unique_lock guard(handleMutex, std::try_to_lock);
    for (int attempt = 0; !guard.owns_lock() && attempt < 50; ++attempt) {
        svcSleepThread(10'000'000ll); // 10 ms
        guard.try_lock();
    }
    if (!guard.owns_lock())
        return;
    for (auto* shared = handles; shared; shared = shared->next)
        if (shared->mode & FsOpenMode_Write)
            fsFileFlush(&shared->handle);
}

// With SQLITE_OS_OTHER, SQLite calls these at sqlite3_initialize / sqlite3_shutdown.
extern "C" int sqlite3_os_init() { return sqlite3_vfs_register(&vfs, 1); }
extern "C" int sqlite3_os_end() { return SQLITE_OK; }