#include <string.h>
#include "atkfind.h"

void LogLine(const char *fmt, ...);

#define CHUNK       0x10000
#define MAX_REGIONS 16

typedef struct {
    u64 addr, size;
    u32 perm;
} Region;

static u8 g_buf[CHUNK + 64];
static Region g_regions[MAX_REGIONS];
static u32 g_num_regions;

static void ListRegions(const DmntCheatProcessMetadata *meta) {
    g_num_regions = 0;
    u64 base = meta->main_nso_extents.base, end = base + meta->main_nso_extents.size;
    for (u64 a = base; a < end && g_num_regions < MAX_REGIONS;) {
        MemoryInfo mi;
        if (R_FAILED(dmntchtQueryCheatProcessMemory(&mi, a)) || mi.size == 0) {
            break;
        }
        if (mi.perm & Perm_R) {
            u64 lo = mi.addr < base ? base : mi.addr, hi = mi.addr + mi.size > end ? end : mi.addr + mi.size;
            g_regions[g_num_regions++] = (Region){ lo, hi - lo, mi.perm };
        }
        a = mi.addr + mi.size;
    }
}

// First address of the bytes in the read-only data, 0 if not found
static u64 FindBytes(const void *bytes, size_t len) {
    for (u32 r = 0; r < g_num_regions; ++r) {
        const Region *g = &g_regions[r];
        if (g->perm != Perm_R) {
            continue;
        }
        for (u64 off = 0; off < g->size; off += CHUNK) {
            u64 n = g->size - off < CHUNK + len ? g->size - off : CHUNK + len;
            if (R_FAILED(dmntchtReadCheatProcessMemory(g->addr + off, g_buf, n))) {
                continue;
            }
            for (u64 i = 0; i + len <= n; ++i) {
                if (g_buf[i] == ((const u8 *)bytes)[0] && memcmp(g_buf + i, bytes, len) == 0) {
                    return g->addr + off + i;
                }
            }
        }
    }
    return 0;
}

// Calls fn(address, value) for every aligned u64 in the data (not code) regions; fn returns true to stop
static u64 ScanQwords(bool (*fn)(u64 addr, u64 value, void *ctx), void *ctx, bool writable_only) {
    for (u32 r = 0; r < g_num_regions; ++r) {
        const Region *g = &g_regions[r];
        if ((g->perm & Perm_X) || (writable_only && !(g->perm & Perm_W))) {
            continue;
        }
        for (u64 off = 0; off < g->size; off += CHUNK) {
            u64 n = g->size - off < CHUNK ? g->size - off : CHUNK;
            if (R_FAILED(dmntchtReadCheatProcessMemory(g->addr + off, g_buf, n))) {
                continue;
            }
            for (u64 i = 0; i + 8 <= n; i += 8) {
                u64 v;
                memcpy(&v, g_buf + i, 8);
                if (fn(g->addr + off + i, v, ctx)) {
                    return g->addr + off + i;
                }
            }
        }
    }
    return 0;
}

static bool IsValue(u64 addr, u64 value, void *ctx) {
    (void)addr;
    return value == *(u64 *)ctx;
}

// vtable of the class whose RTTI name is given: name -> typeinfo (its name field) -> vtable (offset_to_top 0)
typedef struct {
    u64 typeinfo;
} VtableCtx;

static bool IsPrimaryVtable(u64 addr, u64 value, void *ctx) {
    VtableCtx *c = ctx;
    if (value != c->typeinfo) {
        return false;
    }
    u64 offset_to_top;
    return R_SUCCEEDED(dmntchtReadCheatProcessMemory(addr - 8, &offset_to_top, 8)) && offset_to_top == 0;
}

static u64 FindVtable(const char *rtti_name) {
    u64 name = FindBytes(rtti_name, strlen(rtti_name) + 1);
    if (name == 0) {
        return 0;
    }
    u64 name_field = ScanQwords(IsValue, &name, false);    // typeinfo: { vtable of the typeinfo class, name, ... }
    if (name_field == 0) {
        return 0;
    }
    VtableCtx c = { name_field - 8 };
    u64 slot = ScanQwords(IsPrimaryVtable, &c, false);      // vtable: { offset_to_top, typeinfo, functions... }
    return slot ? slot + 8 : 0;
}

typedef struct {
    u64 heap_lo, heap_hi, alias_lo, alias_hi;
    u64 vtable;
    u64 last_target;
} PointerCtx;

static bool InHeap(const PointerCtx *c, u64 v) {
    return (v >= c->heap_lo && v < c->heap_hi) || (v >= c->alias_lo && v < c->alias_hi);
}

// A global that points to an object of the class (its first u64 is the vtable)
static bool PointsToObject(u64 addr, u64 value, void *ctx) {
    (void)addr;
    PointerCtx *c = ctx;
    if (!InHeap(c, value) || (value & 7) || value == c->last_target) {
        return false;
    }
    c->last_target = value;
    u64 first;
    return R_SUCCEEDED(dmntchtReadCheatProcessMemory(value, &first, 8)) && first == c->vtable;
}

bool AtkFind(const DmntCheatProcessMetadata *meta, AtkLocation *out) {
    u64 t0 = armGetSystemTick();
    memset(out, 0, sizeof(*out));
    ListRegions(meta);
    u64 base = meta->main_nso_extents.base;

    u64 wave_vt = FindVtable("N2nn3atk6detail9WaveSoundE");
    u64 sap_vt = FindVtable("N2nn3atk18SoundArchivePlayerE");
    if (wave_vt == 0 || sap_vt == 0) {
        LogLine("atkfind: no nn::atk type names (WaveSound vtable %lX, SoundArchivePlayer vtable %lX): not NintendoWare audio, "
                "or the names were stripped", wave_vt, sap_vt);
        return false;
    }

    PointerCtx pc = {
        meta->heap_extents.base, meta->heap_extents.base + meta->heap_extents.size,
        meta->alias_extents.base, meta->alias_extents.base + meta->alias_extents.size, sap_vt, 0,
    };
    u64 global = ScanQwords(PointsToObject, &pc, true);
    if (global == 0) {
        LogLine("atkfind: vtables at main+%lX / main+%lX, but no global points to a SoundArchivePlayer", wave_vt - base, sap_vt - base);
        return false;
    }
    u64 sap;
    dmntchtReadCheatProcessMemory(global, &sap, 8);

    // The SoundArchivePlayer's fields that point to a WaveSound; the pool starts at the lowest of them
    u8 obj[0x400];
    if (R_FAILED(dmntchtReadCheatProcessMemory(sap, obj, sizeof(obj)))) {
        return false;
    }
    u64 pool = 0, field = 0;
    for (u32 i = 0; i < sizeof(obj); i += 8) {
        u64 v, first;
        memcpy(&v, obj + i, 8);
        if (InHeap(&pc, v) && R_SUCCEEDED(dmntchtReadCheatProcessMemory(v, &first, 8)) && first == wave_vt &&
            (pool == 0 || v <= pool)) {
            pool = v;
            field = i;      // equal values: the later field (list heads come first, the buffer pointer after them)
        }
    }
    if (pool == 0) {
        LogLine("atkfind: SoundArchivePlayer at %lX has no WaveSound pool field", sap);
        return false;
    }

    // Slot size and count: the run of WaveSound vtables after the pool start (read in chunks, up to 1 MB)
    u32 stride = 0, count = 1;
    u64 prev = pool;
    bool done = false;
    for (u64 chunk = 0; chunk < 0x100000 && !done; chunk += CHUNK) {
        if (R_FAILED(dmntchtReadCheatProcessMemory(pool + chunk, g_buf, CHUNK))) {
            break;
        }
        for (u64 i = (chunk == 0 ? 8 : 0); i < CHUNK; i += 8) {
            u64 at = pool + chunk + i, v;
            memcpy(&v, g_buf + i, 8);
            if (v != wave_vt) {
                if (stride && at - prev > stride) {
                    done = true;    // past the last slot
                    break;
                }
                continue;
            }
            u32 d = (u32)(at - prev);
            if (stride == 0) {
                stride = d;
            } else if (d != stride) {
                done = true;
                break;
            }
            prev = at;
            if (++count >= 512) {
                done = true;
                break;
            }
        }
    }
    if (stride == 0) {
        LogLine("atkfind: one WaveSound only at %lX, slot size unknown", pool);
        return false;
    }

    out->sound_archive_player = global - base;
    out->sound_pool = field;
    out->wave_sound_vtable = wave_vt - base;
    out->slot_size = stride;
    out->count = count;
    LogLine("atkfind: found in %lu ms: sound_archive_player = %lX, sound_pool = %lX, wave_sound_vtable = %lX, %u slots of %X",
            armTicksToNs(armGetSystemTick() - t0) / 1000000, out->sound_archive_player, out->sound_pool,
            out->wave_sound_vtable, count, stride);
    return true;
}
