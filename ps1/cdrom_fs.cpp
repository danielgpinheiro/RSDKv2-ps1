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

static uint8_t s_sec[ISO_SECTOR];

static bool readSector(uint32_t lba) {
    return g_ps1CDRom->readSectorsBlocking(lba, 1, s_sec, *g_ps1GPU);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Case-insensitive compare of `name` (RSDKv2 uses lowercase paths) vs an
 * ISO9660 entry name of `len` bytes (stored uppercase, may carry a ";1"
 * version suffix). */
static bool nameMatches(const char *name, const uint8_t *entryName, int len) {
    const char *n = name;
    int         i = 0;
    for (; i < len && *n; ++i, ++n) {
        char e = entryName[i];
        if (e == ';')
            return *n == 0; /* version suffix -> end of name */
        char lo = (*n >= 'A' && *n <= 'Z') ? (*n - 'A' + 'a') : *n;
        char el = (e >= 'A' && e <= 'Z') ? (e - 'A' + 'a') : e;
        if (lo != el)
            return false;
    }
    return *n == 0; /* both names fully consumed */
}

/* Find a root-level file, filling lba/size. Returns false if not found. */
static bool findRootFile(const char *path, uint32_t *outLba, uint32_t *outSize) {
    const char *name = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/')
            name = p + 1;
    }

    if (!readSector(16)) /* Primary Volume Descriptor */
        return false;
    /* Root directory record is at PVD offset 156. */
    uint32_t rootLba  = le32(s_sec + 156 + 2);
    uint32_t rootSize = le32(s_sec + 156 + 10);

    uint32_t remaining = rootSize;
    uint32_t sector    = rootLba;
    while (remaining > 0) {
        if (!readSector(sector))
            return false;
        const uint8_t *p   = s_sec;
        const uint8_t *end = s_sec + ISO_SECTOR;
        while (p < end) {
            uint8_t recLen = p[0];
            if (recLen == 0) { /* padding to sector boundary */
                remaining -= (uint32_t)(end - p);
                break;
            }
            if (recLen < 34) /* malformed */
                return false;
            uint8_t nameLen = p[32];
            if (nameMatches(name, p + 33, nameLen)) {
                *outLba  = le32(p + 2);
                *outSize = le32(p + 10);
                return true;
            }
            p += recLen;
            remaining -= recLen;
        }
        ++sector;
    }
    return false;
}

FILE *fopen(const char *path, const char *mode) {
    (void)mode;
    uint32_t lba, size;
    if (!g_ps1CDRom || !g_ps1GPU)
        return nullptr;
    if (!findRootFile(path, &lba, &size))
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

    uint8_t *dst      = (uint8_t *)ptr;
    size_t   remaining = want;
    while (remaining > 0) {
        uint32_t sector = f->lba + (f->pos / ISO_SECTOR);
        uint32_t off    = f->pos % ISO_SECTOR;
        uint32_t chunk  = ISO_SECTOR - off;
        if (chunk > remaining)
            chunk = (uint32_t)remaining;
        if (!readSector(sector))
            break;
        memcpy(dst, s_sec + off, chunk);
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
