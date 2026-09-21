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

extern psyqo::CDRomDevice *g_ps1CDRom;
extern psyqo::GPU         *g_ps1GPU;

#define ISO_SECTOR 2048
#define SECTOR_BATCH 16 /* read up to 16 consecutive sectors (32KB) per blocking call */

static uint8_t s_sec[ISO_SECTOR];
static uint8_t s_batch[SECTOR_BATCH * ISO_SECTOR];

static bool readSector(uint32_t lba) {
    return g_ps1CDRom->readSectorsBlocking(lba, 1, s_sec, *g_ps1GPU);
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
static bool findFile(const char *path, uint32_t *outLba, uint32_t *outSize) {
    if (!readSector(16)) /* Primary Volume Descriptor */
        return false;
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
                        if (isDir) return false; /* expecting a file */
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

FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    uint32_t lba, size;
    if (!g_ps1CDRom || !g_ps1GPU)
        return nullptr;
    if (!findFile(path, &lba, &size))
        return nullptr;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f)
        return nullptr;
    f->lba   = lba;
    f->size  = size;
    f->pos   = 0;
    f->valid = 1;
    return f;
}

int fclose(FILE *f) {
    if (!f)
        return 0;
    free(f);
    return 0;
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
    while (remaining > 0) {
        uint32_t sector     = f->lba + (f->pos / ISO_SECTOR);
        uint32_t off        = f->pos % ISO_SECTOR;
        uint32_t lastSector = f->lba + (f->size + ISO_SECTOR - 1) / ISO_SECTOR;
        uint32_t sectors    = (off + (uint32_t)remaining + ISO_SECTOR - 1) / ISO_SECTOR;
        if (sectors > SECTOR_BATCH)
            sectors = SECTOR_BATCH;
        uint32_t maxSectors = lastSector - sector;
        if (sectors > maxSectors)
            sectors = maxSectors;
        if (sectors == 0)
            break;
        if (!g_ps1CDRom->readSectorsBlocking(sector, sectors, s_batch, *g_ps1GPU))
            break;
        uint32_t avail = sectors * ISO_SECTOR - off;
        uint32_t chunk = (avail < remaining) ? avail : (uint32_t)remaining;
        memcpy(dst, s_batch + off, chunk);
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
