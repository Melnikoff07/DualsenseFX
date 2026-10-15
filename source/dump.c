// Research helper: copies the game's main module from memory to the SD card (dump_main=1), so code that is not
// decompiled for this game version can be located offline. The file stays on the user's own card.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dump.h"

void LogLine(const char *fmt, ...);

#define DUMP_PATH  "sdmc:/config/dualsensefx/main.bin"
#define DUMP_CHUNK 0x20000  // per poll, ~2.5 MB/s at 20 Hz
#define PAGE       0x1000

bool g_dump_main = false;

static FILE *g_file;
static u8 *g_buf;
static u64 g_base, g_size, g_pos;
static u32 g_unreadable_pages;

void DumpStop(void) {
    if (g_file) {
        fclose(g_file);
        LogLine("dump: interrupted at +%lX of %lX", g_pos, g_size);
    }
    free(g_buf);
    g_file = NULL;
    g_buf = NULL;
}

void DumpStart(const DmntCheatProcessMetadata *meta) {
    if (!g_dump_main || g_file) {
        return;
    }
    g_base = meta->main_nso_extents.base;
    g_size = meta->main_nso_extents.size;

    struct stat st;
    if (stat(DUMP_PATH, &st) == 0 && (u64)st.st_size == g_size) {
        LogLine("dump: main.bin already complete (%lX bytes), delete it to dump again", g_size);
        g_dump_main = false;
        return;
    }

    // Segment map of the main module: text (r-x), rodata (r--), data / bss (rw-)
    for (u64 addr = g_base; addr < g_base + g_size;) {
        MemoryInfo mi;
        if (R_FAILED(dmntchtQueryCheatProcessMemory(&mi, addr)) || mi.size == 0) {
            LogLine("dump: memory query failed at +%lX", addr - g_base);
            break;
        }
        LogLine("dump: map +%08lX size %08lX perm %X type %X", mi.addr - g_base, mi.size, mi.perm, mi.type);
        addr = mi.addr + mi.size;
    }

    g_buf = malloc(DUMP_CHUNK);
    g_file = g_buf ? fopen(DUMP_PATH, "wb") : NULL;
    if (!g_file) {
        LogLine("dump: can't start (no memory or file)");
        free(g_buf);
        g_buf = NULL;
        g_dump_main = false;
        return;
    }
    g_pos = 0;
    g_unreadable_pages = 0;
    LogLine("dump: writing %lX bytes of the main module to %s", g_size, DUMP_PATH);
}

void DumpUpdate(void) {
    if (!g_file) {
        return;
    }

    u64 n = g_size - g_pos < DUMP_CHUNK ? g_size - g_pos : DUMP_CHUNK;
    if (R_FAILED(dmntchtReadCheatProcessMemory(g_base + g_pos, g_buf, n))) {
        // Fall back to single pages, unreadable ones become zeros
        for (u64 off = 0; off < n; off += PAGE) {
            u64 len = n - off < PAGE ? n - off : PAGE;
            if (R_FAILED(dmntchtReadCheatProcessMemory(g_base + g_pos + off, g_buf + off, len))) {
                memset(g_buf + off, 0, len);
                ++g_unreadable_pages;
            }
        }
    }
    if (fwrite(g_buf, 1, n, g_file) != n) {
        LogLine("dump: write failed at +%lX", g_pos);
        DumpStop();
        g_dump_main = false;
        return;
    }
    g_pos += n;

    if (g_pos >= g_size) {
        fclose(g_file);
        g_file = NULL;
        free(g_buf);
        g_buf = NULL;
        g_dump_main = false;
        LogLine("dump: done, %lX bytes, %u unreadable pages", g_size, g_unreadable_pages);
    }
}
