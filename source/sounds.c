// Haptics from the game's own sounds (BotW 1.6.0).
//
// The sound engine is nn::atk (RTTI names kept in the binary). main+2CC1670 holds the SoundArchivePlayer,
// SoundArchivePlayer +128 points to a pool of 64 WaveSound objects, 0x3A0 bytes each (vtable main+2C17828, checked
// before use). Per slot, recorded live: +100 reads FFFFFFFF while the slot is free, +104 is a serial that changes with
// every new sound, +200 points to the FWAV being played.
// BotW keeps its sounds in BARS packages: "BARS", u32 size, u16 BOM, u16 version, u32 count, u32 name hashes[count],
// then { u32 AMTA offset, u32 FWAV offset }[count]. The AMTA metadata has the sound name in its STRG block. The FWAV
// lies inside its package, so the package is found by looking backwards for a BARS header whose table points at it.
// Names seen while playing, for example: Mt_Grass_Pl_MoveFast_HeelL00 (material, player, pace, heel / toe, foot),
// Mt_Stone_Pl_Jump_00, Mt_Wood_Pl_LandWeak_00, Mt_Stone_Pl_ClimbStrong_HandL00, Water_In_SmallSlow01,
// Water_Pl_SwimHand_MdlSizeFast02, Water_Pl_MoveShallowFast02, TboxFieldMetal_Open, Pl_Get02, SYS_ItemGet_Common,
// Pl_FrontRoll00, WoodBox_Break00.
#include <string.h>
#include "sounds.h"
#include "haptics.h"

void LogLine(const char *fmt, ...);
void LogVerbose(const char *fmt, ...);
bool BotwInGame(const BotwState *state);

#define SOUND_ARCHIVE_PLAYER   0x2CC1670
#define SAP_WAVE_POOL          0x128
#define WAVE_SOUND_VTABLE      0x2C17828
#define WAVE_SOUND_SIZE        0x3A0
#define WAVE_SOUND_COUNT       64
#define WAVE_FREE              0x100
#define WAVE_SERIAL            0x104
#define WAVE_DATA              0x200

#define BARS_SEARCH_BACK       0x400000   // packages seen so far start within 4 MB before their FWAVs
#define BARS_CHUNK             0x10000
#define CACHE_SIZE             2048
#define REPEAT_NS              (100 * 1000000ULL)  // layered variants of one sound start together

typedef enum {
    Kind_Unknown = 0,       // looked up, nothing to play
    Kind_Step,
    Kind_Jump,
    Kind_Land,
    Kind_Climb,
    Kind_WaterIn,
    Kind_Swim,
    Kind_Chest,
    Kind_ItemGet,
    Kind_Roll,
    Kind_CrateBreak,
    Kind_Count,
} SoundKind;

typedef struct {
    u64 fwav;               // 0 = empty
    u32 tag;                // FWAV file size, tells a reused address apart
    u8 kind;
    u8 material;            // steps: BotW ground material index
    u8 amount;              // percent
    u8 left;
} CacheEntry;

static u64 g_main_base;
static u64 g_pool;
static u64 g_check_tick;
static bool g_serials_valid;
static u32 g_serials[WAVE_SOUND_COUNT];
static CacheEntry g_cache[CACHE_SIZE];
static u64 g_last_kind_tick[Kind_Count];
static u8 g_pool_buf[WAVE_SOUND_COUNT * WAVE_SOUND_SIZE];
static u8 g_chunk[BARS_CHUNK + 4];
static u32 g_resolved, g_unnamed;

void SoundsReset(void) {
    memset(g_cache, 0, sizeof(g_cache));
    g_serials_valid = false;
}

void SoundsUpdate(const DmntCheatProcessMetadata *meta) {
    u64 now = armGetSystemTick();
    if (now < g_check_tick && meta->main_nso_extents.base == g_main_base) {
        return;
    }
    g_check_tick = now + armNsToTicks(5000 * 1000000ULL);
    u64 base = meta->main_nso_extents.base, sap = 0, pool = 0, first = 0, last = 0;
    bool ok = R_SUCCEEDED(dmntchtReadCheatProcessMemory(base + SOUND_ARCHIVE_PLAYER, &sap, sizeof(sap))) && sap != 0 &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(sap + SAP_WAVE_POOL, &pool, sizeof(pool))) && pool != 0 &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(pool, &first, sizeof(first))) &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(pool + (WAVE_SOUND_COUNT - 1) * WAVE_SOUND_SIZE, &last, sizeof(last))) &&
              first == base + WAVE_SOUND_VTABLE && last == base + WAVE_SOUND_VTABLE;
    if (!ok) {
        pool = 0;
    }
    if (pool != g_pool) {
        LogLine(ok ? "sounds: pool at %010lX, sound haptics on" : "sounds: pool not found (%010lX), sound haptics off", pool);
        SoundsReset();
    }
    g_pool = pool;
    g_main_base = base;
}

// ---- names ----

static bool StartsWith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static u8 MaterialFromName(const char *m, size_t len) {
    static const struct { const char *prefix; u8 material; } Map[] = {
        { "GrassWithMud", 1 }, { "Grass", 6 }, { "Soil", 1 }, { "Mud", 1 }, { "Sand", 3 }, { "Stone", 2 }, { "Rock", 2 },
        { "Wood", 7 }, { "Metal", 4 }, { "Ice", 10 }, { "Snow", 9 }, { "Cloth", 14 },
    };
    for (size_t i = 0; i < sizeof(Map) / sizeof(Map[0]); ++i) {
        size_t n = strlen(Map[i].prefix);
        if (len >= n && strncmp(m, Map[i].prefix, n) == 0) {
            return Map[i].material;
        }
    }
    return 1;
}

// Pace word after "Move" / "MoveShallow": Slow, Normal, Fast, Naked (barefoot)
static u8 PaceAmount(const char *p) {
    if (StartsWith(p, "Fast"))   return 100;
    if (StartsWith(p, "Normal")) return 75;
    if (StartsWith(p, "Naked"))  return 70;
    return 55;
}

static void Classify(const char *name, CacheEntry *e) {
    e->kind = Kind_Unknown;
    e->amount = 100;
    e->left = 0;
    if (StartsWith(name, "Mt_")) {
        const char *m = name + 3;
        const char *pl = strstr(m, "_Pl_");
        if (pl == NULL) {
            return;     // a material sound of something else than the player
        }
        e->material = MaterialFromName(m, pl - m);
        const char *what = pl + 4;
        if (StartsWith(what, "Move")) {
            const char *pace = what + 4;
            const char *part = strchr(pace, '_');
            e->kind = Kind_Step;
            e->amount = PaceAmount(pace);
            if (part != NULL) {
                ++part;
                if (StartsWith(part, "Toe")) {
                    e->amount = e->amount * 4 / 10;   // the toe follows the heel, a light second touch
                } else if (StartsWith(part, "HeelAccent")) {
                    e->amount = e->amount * 8 / 10;
                }
                const char *foot = part + (StartsWith(part, "Heel") ? 4 : StartsWith(part, "Toe") ? 3 : 0);
                e->left = *foot == 'L';
            }
        } else if (StartsWith(what, "Jump")) {
            e->kind = Kind_Jump;
        } else if (StartsWith(what, "Land")) {
            e->kind = Kind_Land;
            e->amount = strstr(what, "Weak") != NULL ? 0 : 100;   // 0 = light landing, else a real fall
        } else if (StartsWith(what, "Climb")) {
            e->kind = Kind_Climb;
            e->left = strstr(what, "L0") != NULL;
        }
    } else if (StartsWith(name, "Water_Pl_MoveShallow")) {
        e->kind = Kind_Step;
        e->material = 8;
        e->amount = PaceAmount(name + 20);
    } else if (StartsWith(name, "Water_Pl_SwimHand")) {
        e->kind = Kind_Swim;
    } else if (StartsWith(name, "Water_In_")) {
        e->kind = Kind_WaterIn;
        e->amount = StartsWith(name + 9, "Small") ? 60 : 100;
    } else if (StartsWith(name, "Tbox") && strstr(name, "Open") != NULL) {
        e->kind = Kind_Chest;
    } else if (StartsWith(name, "Pl_Get") || StartsWith(name, "SYS_ItemGet")) {
        e->kind = Kind_ItemGet;
    } else if (StartsWith(name, "Pl_FrontRoll")) {
        e->kind = Kind_Roll;
    } else if (StartsWith(name, "WoodBox_Break")) {
        e->kind = Kind_CrateBreak;
    }
}

// Name of the sound in its package's AMTA STRG block
static bool AmtaName(u64 amta, char *out, size_t size) {
    u8 h[0x100];
    if (R_FAILED(dmntchtReadCheatProcessMemory(amta, h, sizeof(h))) || memcmp(h, "AMTA", 4) != 0) {
        return false;
    }
    for (size_t i = 4; i + 8 < sizeof(h); i += 4) {
        if (memcmp(h + i, "STRG", 4) == 0) {
            u32 len;
            memcpy(&len, h + i + 4, sizeof(len));
            size_t n = len < size - 1 ? len : size - 1;
            if (i + 8 + n > sizeof(h)) {
                n = sizeof(h) - i - 8;
            }
            memcpy(out, h + i + 8, n);
            out[n] = 0;
            return out[0] != 0;
        }
    }
    return false;
}

// Is there a BARS header at bars whose table lists this FWAV? Then fetch the name.
static bool NameFromBars(u64 bars, u64 fwav, char *name, size_t size) {
    u32 count;
    if (R_FAILED(dmntchtReadCheatProcessMemory(bars + 0xC, &count, sizeof(count))) || count == 0 || count > 5000) {
        return false;
    }
    u64 table = bars + 0x10 + 4ULL * count;
    u32 entries[2 * 128];
    for (u32 first = 0; first < count; first += 128) {
        u32 n = count - first < 128 ? count - first : 128;
        if (R_FAILED(dmntchtReadCheatProcessMemory(table + 8ULL * first, entries, 8 * n))) {
            return false;
        }
        for (u32 k = 0; k < n; ++k) {
            if (bars + entries[2 * k + 1] == fwav) {
                return AmtaName(bars + entries[2 * k], name, size);
            }
        }
    }
    return false;
}

static bool LookUpName(u64 fwav, char *name, size_t size) {
    for (u64 hi = fwav; hi > fwav - BARS_SEARCH_BACK; hi -= BARS_CHUNK) {
        u64 lo = hi - BARS_CHUNK;
        if (R_FAILED(dmntchtReadCheatProcessMemory(lo, g_chunk, sizeof(g_chunk)))) {
            return false;
        }
        for (s64 i = BARS_CHUNK; i >= 0; i -= 4) {     // BARS headers are 4-byte aligned
            if (memcmp(g_chunk + i, "BARS", 4) == 0 && NameFromBars(lo + i, fwav, name, size)) {
                return true;
            }
        }
    }
    return false;
}

static CacheEntry *Find(u64 fwav, u32 tag, bool *found) {
    u32 h = (u32)((fwav >> 4) * 2654435761u) % CACHE_SIZE;
    for (u32 n = 0; n < CACHE_SIZE; ++n) {
        CacheEntry *e = &g_cache[(h + n) % CACHE_SIZE];
        if (e->fwav == fwav && e->tag == tag) {
            *found = true;
            return e;
        }
        if (e->fwav == 0 || e->fwav == fwav) {      // free, or the same address now holding another sound
            *found = false;
            return e;
        }
    }
    SoundsReset();      // full: start over
    *found = false;
    return &g_cache[h];
}

// ---- events ----

static void Play(BotwState *state, const CacheEntry *e) {
    if (e->kind == Kind_Unknown || !BotwInGame(state) || state->menu_open) {
        return;
    }
    u64 now = armGetSystemTick();
    if (e->kind == Kind_Step) {
        HapticsPlayStep(e->material, e->left, e->amount / 100.0f);
        return;
    }
    if (now - g_last_kind_tick[e->kind] < armNsToTicks(REPEAT_NS)) {
        return;
    }
    g_last_kind_tick[e->kind] = now;
    if (e->kind == Kind_Land && e->amount == 100) {
        HapticsPlay(HapticPattern_Landing, 1.0f);
        return;
    }
    static const HapticPattern Patterns[Kind_Count] = {
        [Kind_Jump] = HapticPattern_Jump, [Kind_Land] = HapticPattern_LandSoft, [Kind_Climb] = HapticPattern_ClimbGrip,
        [Kind_WaterIn] = HapticPattern_WaterSplash, [Kind_Swim] = HapticPattern_SwimStroke, [Kind_Chest] = HapticPattern_ChestOpen,
        [Kind_ItemGet] = HapticPattern_ItemGet, [Kind_Roll] = HapticPattern_Roll, [Kind_CrateBreak] = HapticPattern_CrateBreak,
    };
    HapticsPlay(Patterns[e->kind], e->kind == Kind_Land ? 1.0f : e->amount / 100.0f);
}

void SoundsPoll(BotwState *state) {
    if (g_pool == 0 || !g_haptic_settings.enabled ||
        R_FAILED(dmntchtReadCheatProcessMemory(g_pool, g_pool_buf, sizeof(g_pool_buf)))) {
        g_serials_valid = false;
        return;
    }
    bool looked_up = false;     // at most one (slow) name lookup per poll
    for (u32 i = 0; i < WAVE_SOUND_COUNT; ++i) {
        const u8 *slot = g_pool_buf + i * WAVE_SOUND_SIZE;
        u32 free_mark, serial;
        u64 fwav;
        memcpy(&free_mark, slot + WAVE_FREE, sizeof(free_mark));
        memcpy(&serial, slot + WAVE_SERIAL, sizeof(serial));
        memcpy(&fwav, slot + WAVE_DATA, sizeof(fwav));
        bool started = g_serials_valid && free_mark != 0xFFFFFFFF && serial != g_serials[i];
        g_serials[i] = serial;
        if (!started || fwav == 0) {
            continue;
        }

        u8 head[16];
        if (R_FAILED(dmntchtReadCheatProcessMemory(fwav, head, sizeof(head))) || memcmp(head, "FWAV", 4) != 0) {
            continue;
        }
        u32 tag;
        memcpy(&tag, head + 0xC, sizeof(tag));
        bool found;
        CacheEntry *e = Find(fwav, tag, &found);
        if (!found) {
            if (looked_up) {
                continue;
            }
            looked_up = true;
            char name[64];
            memset(e, 0, sizeof(*e));
            e->fwav = fwav;
            e->tag = tag;
            if (LookUpName(fwav, name, sizeof(name))) {
                Classify(name, e);
                ++g_resolved;
                if (e->kind != Kind_Unknown) {
                    LogVerbose("sounds: %s -> kind %u material %u amount %u", name, e->kind, e->material, e->amount);
                }
            } else {
                ++g_unnamed;
            }
        }
        Play(state, e);
    }
    g_serials_valid = true;
}
