/*
 * PS1 port — CD-ROM file I/O (Phase 6)
 *
 * Synchronous ISO9660 reader on top of psyqo::CDRomDevice::readSectorsBlocking,
 * exposed as the standard <stdio.h> FILE* API so the untouched RSDKv2 Reader.cpp
 * (fopen/fread/fseek/ftell/fclose + ParseVirtualFileSystem) works unchanged.
 *
 * Only root-level files are looked up (sufficient for "Data.bin", the single
 * virtual-filesystem file the engine reads). Nested paths (loose Data/ folder
 * fallback) are deferred.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psyqo/cdrom-device.hh>
#include <psyqo/gpu.hh>
#include "common/syscalls/syscalls.h"

#include "video.hh"

extern psyqo::CDRomDevice *g_ps1CDRom;
volatile uint32_t g_ps1VideoForcedClose = 0; // file reads that had to stop an open FMV stream
extern psyqo::GPU         *g_ps1GPU;

#define ISO_SECTOR 2048
#define DIRECT_MAX   32 /* sector-aligned reads straight into the caller's buffer: up to 64KB per call */

static uint8_t *s_sec;

/* Read-ahead windows (load-time phase). Every blocking request costs a Setloc/ReadN(seek)/Pause
 * cycle, which dominates load times (the engine refills 8 KB at a time and re-opens config files
 * over and over). A buffered read that misses fetches WINDOW_SECTORS sectors at once and later
 * reads are served from RAM. The window runs past the file's end: tools/disc/build_disc.py lays
 * the data files out in load order, so it usually holds the next files the game opens (the
 * CD-XA/STR streams sit after all data; read as data they are just unused sectors). Three LRU
 * slots (the engine re-reads GameConfig between the SFX it loads), heap-allocated on first use,
 * freed after each load
 * (PS1FsReleaseWindows from PS1LoadingEnd) or at fclose outside a load. */
#define WINDOW_SECTORS 32
#define WINDOWS        3
struct Window {
    uint8_t *buf;
    uint32_t lba, count, age;
};
static Window s_win[WINDOWS];
static bool readViaWindows(uint32_t lba, uint32_t bytes, uint8_t *dst);
static uint32_t s_winClock    = 0;
static uint32_t s_volumeEnd   = 0; // sectors in the volume (PVD): read-ahead never goes past it
volatile uint32_t g_ps1FsWindowHits  = 0; // buffered reads served from a window (GDB)
volatile uint32_t g_ps1FsWindowFills = 0; // window misses (one CD request each)

/* findFile reads the volume descriptor and every directory on the path for each fopen; the disc
 * is read-only, so those sectors are cached (LRU) instead of costing a blocking CD request each. */
#define DIR_CACHE 8
static uint8_t s_dirCache[DIR_CACHE][ISO_SECTOR];
static uint32_t s_dirCacheLba[DIR_CACHE];
static uint32_t s_dirCacheAge[DIR_CACHE];
static uint32_t s_dirCacheClock = 0;
volatile uint32_t g_ps1FsDirReads = 0; // directory sectors read from the CD (cache misses, GDB)
volatile uint32_t g_ps1FsDirHits  = 0;

volatile uint32_t g_ps1FsRequests = 0; // blocking CD requests (each = Setloc/ReadN/Pause), GDB
volatile uint32_t g_ps1FsSectors  = 0; // sectors those requests read

volatile uint32_t g_ps1FsReadUs   = 0; // time spent inside them (microseconds)

// GDB profiling point (tools/load_profile.sh): one call per request, after it completed.
extern "C" __attribute__((noinline)) void PS1FsProfileHook(uint32_t lba, uint32_t count, uint32_t us) {
    __asm__ volatile("" ::"r"(lba), "r"(count), "r"(us));
}

// Every file-system CD read goes through here (counted for load profiling).
static bool cdRead(uint32_t lba, uint32_t count, void *dst) {
    uint32_t t0 = g_ps1GPU->now();
    bool ok     = g_ps1CDRom->readSectorsBlocking(lba, count, dst, *g_ps1GPU);
    uint32_t us = g_ps1GPU->now() - t0;
    g_ps1FsRequests = g_ps1FsRequests + 1;
    g_ps1FsSectors  = g_ps1FsSectors + count;
    g_ps1FsReadUs   = g_ps1FsReadUs + us;
    PS1FsProfileHook(lba, count, us);
    return ok;
}

static bool readSector(uint32_t lba) {
    int victim = 0;
    for (int i = 0; i < DIR_CACHE; ++i) {
        if (s_dirCacheAge[i] && s_dirCacheLba[i] == lba) {
            s_dirCacheAge[i] = ++s_dirCacheClock;
            s_sec            = s_dirCache[i];
            g_ps1FsDirHits   = g_ps1FsDirHits + 1;
            return true;
        }
        if (s_dirCacheAge[i] < s_dirCacheAge[victim])
            victim = i;
    }
    if (!cdRead(lba, 1, s_dirCache[victim])) {
        s_dirCacheAge[victim] = 0;
        return false;
    }
    s_dirCacheLba[victim] = lba;
    s_dirCacheAge[victim] = ++s_dirCacheClock;
    s_sec                 = s_dirCache[victim];
    g_ps1FsDirReads       = g_ps1FsDirReads + 1;
    return true;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Case-insensitive compare of a bounded path component `name` (nameLen bytes)
 * vs an ISO9660 entry name of entryLen bytes (stored uppercase, may carry a
 * ";1" version suffix). */
static bool nameMatchesBounded(const char *name, int nameLen, const uint8_t *entryName, int entryLen) {
    int i = 0, j = 0;
    while (i < nameLen && j < entryLen) {
        char e = (char)entryName[j];
        if (e == ';')
            return i == nameLen; /* version suffix -> component must end here */
        char lo = name[i];
        if (lo >= 'A' && lo <= 'Z') lo += 'a' - 'A';
        char el = e;
        if (el >= 'A' && el <= 'Z') el += 'a' - 'A';
        if (lo != el)
            return false;
        ++i; ++j;
    }
    return i == nameLen; /* component fully consumed */
}

/* Find a file by a nested path (e.g. "Data/Stages/SSZ/Act1.bin"), walking the
 * ISO9660 directory tree. Fills lba/size. Returns false if not found. */
static bool findEntry(const char *path, uint32_t *outLba, uint32_t *outSize, bool wantDir) {
    if (!readSector(16)) /* Primary Volume Descriptor */
        return false;
    s_volumeEnd = le32(s_sec + 80); /* volume space size */
    /* Root directory record is at PVD offset 156. */
    uint32_t dirLba  = le32(s_sec + 156 + 2);
    uint32_t dirSize = le32(s_sec + 156 + 10);

    const char *p = path;
    while (true) {
        const char *name = p;
        while (*p && *p != '/') ++p;
        int  nameLen = (int)(p - name);
        bool last    = (*p == 0);
        if (*p == '/') ++p;

        if (nameLen == 0) { /* empty component (leading/trailing slash) */
            if (last) return false;
            continue;
        }

        bool     found     = false;
        uint32_t remaining = dirSize;
        uint32_t sector    = dirLba;
        while (remaining > 0 && !found) {
            if (!readSector(sector))
                return false;
            const uint8_t *q   = s_sec;
            const uint8_t *end = s_sec + ISO_SECTOR;
            while (q < end) {
                uint8_t recLen = q[0];
                if (recLen == 0) { /* padding to sector boundary */
                    remaining -= (uint32_t)(end - q);
                    break;
                }
                if (recLen < 34) /* malformed */
                    return false;
                uint8_t nlen = q[32];
                if (nameMatchesBounded(name, nameLen, q + 33, nlen)) {
                    uint32_t lba   = le32(q + 2);
                    uint32_t sz    = le32(q + 10);
                    uint8_t  flags = q[25];
                    bool     isDir = (flags & 2) != 0;
                    if (last) {
                        if (isDir != wantDir) return false; /* file vs directory */
                        *outLba = lba; *outSize = sz;
                        return true;
                    }
                    if (!isDir) return false; /* expecting a directory */
                    dirLba = lba; dirSize = sz;
                    found = true;
                    break;
                }
                q += recLen;
                remaining -= recLen;
            }
            ++sector;
        }
        if (!found)
            return false;
    }
}

static bool findFile(const char *path, uint32_t *outLba, uint32_t *outSize) { return findEntry(path, outLba, outSize, false); }
static bool findDir(const char *path, uint32_t *outLba, uint32_t *outSize) { return findEntry(path, outLba, outSize, true); }

bool PS1FsFind(const char *path, uint32_t *lba, uint32_t *size) { return findFile(path, lba, size); }

bool PS1XaStopForFileRead(); // xa_music.cpp

/* Resident files: small files the scripts read during play (LoadPalette), preloaded into RAM
 * so reading them never touches the drive, which may be streaming CD-XA music. */
#define RESIDENT_MAX 16
struct Resident {
    char path[48];
    uint8_t *data;
    uint32_t size;
};
static Resident s_resident[RESIDENT_MAX];
static int s_residentCount = 0;
volatile uint32_t g_ps1FsResidentReads = 0; // fopen served from RAM (GDB)

static bool pathEq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* Preload every file of `dir` (e.g. "Data/Palettes") up to maxSize bytes. Returns the count. */
int PS1FsPreloadDir(const char *dir, uint32_t maxSize) {
    uint32_t dirLba, dirSize;
    if (!findDir(dir, &dirLba, &dirSize))
        return 0;
    int loaded = 0;
    for (uint32_t off = 0; off < dirSize; off += ISO_SECTOR) {
        if (!readSector(dirLba + off / ISO_SECTOR))
            break;
        static uint8_t sec[ISO_SECTOR]; // s_sec may be evicted by nested reads; not on the stack (2 KB)
        memcpy(sec, s_sec, ISO_SECTOR);
        for (const uint8_t *q = sec; q < sec + ISO_SECTOR && q[0];) {
            uint8_t recLen = q[0], nlen = q[32];
            uint32_t lba = le32(q + 2), sz = le32(q + 10);
            bool isDir = (q[25] & 2) != 0;
            if (!isDir && nlen > 1 && sz <= maxSize && s_residentCount < RESIDENT_MAX) {
                Resident &r = s_resident[s_residentCount];
                int n = snprintf(r.path, sizeof(r.path), "%s/", dir);
                for (int i = 0; i < nlen && q[33 + i] != ';' && n < (int)sizeof(r.path) - 1; ++i) r.path[n++] = (char)q[33 + i];
                r.path[n] = 0;
                r.data = (uint8_t *)malloc(sz ? sz : 1);
                if (r.data && readViaWindows(lba, sz, r.data)) { // contiguous files: few requests
                    r.size = sz;
                    ++s_residentCount;
                    ++loaded;
                }
            }
            q += recLen;
        }
    }
    return loaded;
}

FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    uint32_t lba, size;
    if (!g_ps1CDRom || !g_ps1GPU)
        return nullptr;
    if (PS1VideoIsOpen()) {
        // One CD action at a time: a file read while the FMV streams would break both.
        // Callers stop the video first (PS1StopVideo); this is the safety net.
        ramsyscall_printf("[cdrom_fs] fopen('%s') while a video streams: closing the video\n", path);
        g_ps1VideoForcedClose = g_ps1VideoForcedClose + 1;
        PS1VideoClose();
    }
    for (int i = 0; i < s_residentCount; ++i) {
        if (pathEq(s_resident[i].path, path)) {
            FILE *f = (FILE *)malloc(sizeof(FILE));
            if (!f)
                return nullptr;
            f->lba   = 0;
            f->size  = s_resident[i].size;
            f->pos   = 0;
            f->valid = 1;
            f->mem   = s_resident[i].data;
            g_ps1FsResidentReads = g_ps1FsResidentReads + 1;
            return f;
        }
    }
    // One CD action at a time: CD-XA music reads continuously; scripts stop it before loads,
    // this is the safety net (counted in g_ps1XaForcedStops).
    PS1XaStopForFileRead();
    if (!findFile(path, &lba, &size))
        return nullptr;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f)
        return nullptr;
    f->lba   = lba;
    f->size  = size;
    f->pos   = 0;
    f->valid  = 1;
    f->mem    = nullptr;
    return f;
}

void PS1FsReleaseWindows() {
    for (auto &w : s_win) {
        free(w.buf);
        w = Window{};
    }
}

bool PS1LoadingActive(); // loading.cpp

int fclose(FILE *f) {
    if (!f)
        return 0;
    free(f);
    if (!PS1LoadingActive())
        PS1FsReleaseWindows(); // reads outside a bracketed load don't keep 192 KB of heap
    return 0;
}

// The window holding `sector`, filling the least recently used one on a miss (one CD request of
// up to WINDOW_SECTORS sectors).
static Window *windowFor(uint32_t sector);

static bool readViaWindows(uint32_t lba, uint32_t bytes, uint8_t *dst) {
    for (uint32_t pos = 0; pos < bytes;) {
        Window *w = windowFor(lba + pos / ISO_SECTOR);
        if (!w)
            return false;
        uint32_t skip  = (lba - w->lba) * ISO_SECTOR + pos;
        uint32_t chunk = w->count * ISO_SECTOR - skip;
        if (chunk > bytes - pos)
            chunk = bytes - pos;
        memcpy(dst + pos, w->buf + skip, chunk);
        pos += chunk;
    }
    return true;
}

static Window *windowFor(uint32_t sector) {
    Window *victim = &s_win[0];
    for (auto &w : s_win) {
        if (w.buf && w.count && sector >= w.lba && sector < w.lba + w.count) {
            w.age            = ++s_winClock;
            g_ps1FsWindowHits = g_ps1FsWindowHits + 1;
            return &w;
        }
        if (w.age < victim->age)
            victim = &w;
    }
    if (!victim->buf && !(victim->buf = (uint8_t *)malloc(WINDOW_SECTORS * ISO_SECTOR)))
        return nullptr;
    uint32_t count = s_volumeEnd > sector ? s_volumeEnd - sector : 1;
    if (count > WINDOW_SECTORS)
        count = WINDOW_SECTORS;
    victim->count = 0;
    if (!cdRead(sector, count, victim->buf))
        return nullptr;
    victim->lba        = sector;
    victim->count      = count;
    victim->age        = ++s_winClock;
    g_ps1FsWindowFills = g_ps1FsWindowFills + 1;
    return victim;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !f->valid)
        return 0;
    size_t want = size * nmemb;
    if (f->pos >= f->size)
        return 0;
    if (f->pos + want > f->size)
        want = f->size - f->pos;

    uint8_t *dst       = (uint8_t *)ptr;
    size_t   remaining = want;
    if (f->mem) {
        memcpy(dst, f->mem + f->pos, remaining);
        f->pos += (uint32_t)remaining;
        return want / size;
    }
    while (remaining > 0) {
        uint32_t sector = f->lba + (f->pos / ISO_SECTOR);
        uint32_t off    = f->pos % ISO_SECTOR;
        // Big sector-aligned reads (a whole window or more) go straight into the caller's buffer:
        // one request of up to DIRECT_MAX sectors, no copy.
        if (off == 0 && remaining >= WINDOW_SECTORS * ISO_SECTOR && !((uintptr_t)dst & 3)) {
            uint32_t n = (uint32_t)(remaining / ISO_SECTOR);
            if (n > DIRECT_MAX)
                n = DIRECT_MAX;
            if (!cdRead(sector, n, dst))
                break;
            dst += n * ISO_SECTOR;
            f->pos += n * ISO_SECTOR;
            remaining -= n * ISO_SECTOR;
            continue;
        }
        Window *w = windowFor(sector);
        if (!w)
            break;
        uint32_t skip  = (sector - w->lba) * ISO_SECTOR + off;
        uint32_t avail = w->count * ISO_SECTOR - skip;
        uint32_t chunk = (avail < remaining) ? avail : (uint32_t)remaining;
        memcpy(dst, w->buf + skip, chunk);
        dst += chunk;
        f->pos += chunk;
        remaining -= chunk;
    }
    return (want - remaining) / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)f;
    return 0; /* read-only disc; saves move to the memory card (Phase 8) */
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || !f->valid)
        return -1;
    long base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (long)f->pos; break;
        case SEEK_END: base = (long)f->size; break;
        default: return -1;
    }
    long np = base + offset;
    if (np < 0)
        np = 0;
    if ((uint32_t)np > f->size)
        np = (long)f->size;
    f->pos = (uint32_t)np;
    return 0;
}

long ftell(FILE *f) {
    if (!f || !f->valid)
        return -1;
    return (long)f->pos;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int feof(FILE *f) {
    return !f || !f->valid || f->pos >= f->size;
}

int ferror(FILE *f) {
    (void)f;
    return 0;
}

void clearerr(FILE *f) {
    (void)f;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1, 1, f) == 1)
        return c;
    return EOF;
}

int fputc(int c, FILE *f) {
    (void)c;
    (void)f;
    return EOF;
}

int getc(FILE *f) { return fgetc(f); }
int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
    (void)f;
    if (s)
        syscall_puts(s);
    return 0;
}

int puts(const char *s) {
    if (s)
        syscall_puts(s);
    syscall_puts("\n");
    return 0;
}

char *fgets(char *s, int n, FILE *f) {
    if (!s || n <= 0 || !f)
        return nullptr;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0)
                return nullptr;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return s;
}

void perror(const char *s) {
    if (s)
        syscall_puts(s);
    syscall_puts("\n");
}
