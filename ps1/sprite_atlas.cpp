/*
 * Per-stage sprite atlas loader (see sprite_atlas.hh and tools/atlas/build_atlas.py).
 */
#include "sprite_atlas.hh"
#include "gpu_chain.hh"

#include <stdio.h>
#include <string.h>

#include <psyqo/alloc.h>
#include <psyqo/gpu.hh>

#include "common/syscalls/syscalls.h"

extern psyqo::GPU *g_ps1GPU;

PS1AtlasSheet   g_ps1AtlasSheets[PS1_ATLAS_MAX_SHEETS];
PS1AtlasCluster g_ps1AtlasClusters[PS1_ATLAS_MAX_CLUSTERS];
PS1AtlasClut    g_ps1AtlasCluts[PS1_ATLAS_MAX_CLUTS];
int             g_ps1AtlasSheetCount = 0, g_ps1AtlasClusterCount = 0, g_ps1AtlasClutCount = 0;
uint16_t        g_ps1AtlasFmvX = 0xFFFF, g_ps1AtlasFmvY = 0xFFFF;

volatile uint32_t g_ps1AtlasLoads      = 0;
volatile uint32_t g_ps1AtlasBlockBytes = 0;
volatile uint32_t g_ps1AtlasMissing    = 0;

struct AtlasHeader {
    char magic[4];
    uint16_t sheets, clusters, cluts, blocks, fmvX, fmvY;
    uint32_t dataOffset;
};
struct AtlasBlock {
    uint16_t x, y, w, h;
    uint32_t offset;
};
static_assert(sizeof(AtlasHeader) == 20, "atlas header");
static_assert(sizeof(AtlasBlock) == 12, "atlas block record");

#define STAGING_BYTES (16 * 1024) // VRAM upload staging (heap, only during the load)
#define MAX_BLOCKS    32

static void clearTables() {
    g_ps1AtlasSheetCount = g_ps1AtlasClusterCount = g_ps1AtlasClutCount = 0;
    g_ps1AtlasFmvX = g_ps1AtlasFmvY = 0xFFFF;
}

bool PS1AtlasLoad(const char *folder) {
    clearTables();
    char path[96];
    snprintf(path, sizeof(path), "Data/Sprites/Atlas/%s.atl", folder);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ramsyscall_printf("[atlas] missing %s: no sprites in this stage\n", path);
        return false;
    }
    AtlasHeader h;
    AtlasBlock blocks[MAX_BLOCKS];
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "ATL1", 4) && h.sheets <= PS1_ATLAS_MAX_SHEETS &&
              h.clusters <= PS1_ATLAS_MAX_CLUSTERS && h.cluts <= PS1_ATLAS_MAX_CLUTS && h.blocks <= MAX_BLOCKS;
    ok = ok && fread(g_ps1AtlasSheets, sizeof(PS1AtlasSheet), h.sheets, f) == h.sheets;
    ok = ok && fread(g_ps1AtlasClusters, sizeof(PS1AtlasCluster), h.clusters, f) == h.clusters;
    ok = ok && fread(g_ps1AtlasCluts, sizeof(PS1AtlasClut), h.cluts, f) == h.cluts;
    ok = ok && fread(blocks, sizeof(AtlasBlock), h.blocks, f) == h.blocks;
    uint16_t *staging = ok ? (uint16_t *)psyqo_malloc(STAGING_BYTES) : nullptr;
    uint32_t bytes    = 0;
    if (staging) {
        PS1GpuImmediate(); // the previous frame's chain may still be texturing from VRAM
        for (int b = 0; ok && b < h.blocks; ++b) {
            const AtlasBlock &blk = blocks[b];
            int linesPerChunk     = STAGING_BYTES / (blk.w * 2);
            ok = fseek(f, (long)(h.dataOffset + blk.offset), SEEK_SET) == 0;
            for (int line = 0; ok && line < blk.h; line += linesPerChunk) {
                int n  = blk.h - line < linesPerChunk ? blk.h - line : linesPerChunk;
                size_t sz = (size_t)blk.w * n * 2;
                ok = fread(staging, 1, sz, f) == sz;
                if (ok)
                    g_ps1GPU->uploadToVRAM(staging, psyqo::Rect{{(int16_t)blk.x, (int16_t)(blk.y + line)},
                                                                {(int16_t)blk.w, (int16_t)n}});
                bytes += sz;
            }
        }
        psyqo_free(staging);
    } else {
        ok = false;
    }
    fclose(f);
    if (!ok) {
        ramsyscall_printf("[atlas] bad or unreadable %s\n", path);
        clearTables();
        return false;
    }
    g_ps1AtlasSheetCount   = h.sheets;
    g_ps1AtlasClusterCount = h.clusters;
    g_ps1AtlasClutCount    = h.cluts;
    g_ps1AtlasFmvX         = h.fmvX;
    g_ps1AtlasFmvY         = h.fmvY;
    g_ps1AtlasBlockBytes   = bytes;
    g_ps1AtlasLoads        = g_ps1AtlasLoads + 1;
    return true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

int PS1AtlasFindSheet(const char *path) {
    static const char prefix[] = "data/sprites/";
    const char *p = path;
    int i = 0;
    while (prefix[i] && lower(p[i]) == prefix[i]) ++i;
    if (!prefix[i])
        p += i; // strip "Data/Sprites/"
    for (int s = 0; s < g_ps1AtlasSheetCount; ++s) {
        const char *n = g_ps1AtlasSheets[s].name;
        int k = 0;
        while (n[k] && lower(p[k]) == n[k]) ++k;
        if (!n[k] && !p[k])
            return s;
    }
    g_ps1AtlasMissing = g_ps1AtlasMissing + 1;
    return -1;
}
