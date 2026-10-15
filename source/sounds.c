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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sounds.h"
#include "haptics.h"
#include "atkfind.h"

void LogLine(const char *fmt, ...);
void LogVerbose(const char *fmt, ...);

#define MAX_SLOTS              256
#define POOL_BUF_SIZE          0x20000
#define WAVE_FREE              0x100
#define WAVE_SERIAL            0x104
#define WAVE_DATA              0x200

#define BARS_SEARCH_BACK       0x400000   // packages seen so far start within 4 MB before their FWAVs
#define BARS_CHUNK             0x10000
#define CACHE_SIZE             1024
#define NAME_SIZE              40
#define MONITOR_ENTRIES        256
#define REPEAT_NS              (100 * 1000000ULL)  // layered variants of one sound start together

typedef enum {
    Kind_Unknown = 0,       // looked up, nothing to play
    Kind_Step,              // footstep: ground, pace and foot from the name
    Kind_Pattern,           // a pattern from patterns.ini
} SoundKind;

// sounds.ini: "name mask = action [strength %]", first matching line wins
typedef struct {
    char mask[64];
    u8 kind;
    s16 pattern;
    u8 amount;
    u8 material;            // steps: forced ground, 0 = from the name
} Rule;

#define MAX_RULES 160

static Rule g_rules[MAX_RULES];
static u32 g_num_rules;

static const char *DefaultRules =
    "; Which game sounds play which haptic pattern. Applied within a second after saving (no reboot).\n"
    "; mask = action [strength %]   '*' matches anything, the first matching line wins.\n"
    "; action: a pattern name from patterns.ini, \"step\" (footstep: ground, pace, heel / toe and foot from the name),\n"
    ";         \"step <ground>\" (footstep on that ground), or \"ignore\".\n"
    "; Sound names can be seen with the sound log tool (tools/sndlog.py) while playing.\n"
    "\n"
    "; Footsteps: Mt_<ground>_Pl_Move<Slow|Normal|Fast|Naked>_<Heel|Toe><L|R>NN\n"
    "Mt_*_Pl_Move* = step\n"
    "Water_Pl_MoveShallow* = step water\n"
    "\n"
    "; Body\n"
    "Mt_*_Pl_Land*Weak* = land\n"
    "Mt_*_Pl_Land* = landing\n"
    "Mt_*_Pl_Jump* = jump\n"
    "Mt_*_Pl_Climb* = climb grip\n"
    "Pl_FrontRoll* = roll\n"
    "\n"
    "; Water\n"
    "Water_In_Small* = water splash 60\n"
    "Water_In_* = water splash\n"
    "Water_Pl_SwimHand* = swim stroke\n"
    "\n"
    "; World\n"
    "Tbox*Open* = chest open\n"
    "Pl_Get* = item get\n"
    "SYS_ItemGet* = item get\n"
    "WoodBox_Break* = crate break\n";


typedef struct {
    u64 fwav;               // 0 = empty
    u32 tag;                // FWAV file size, tells a reused address apart
    u8 kind;
    u8 material;            // steps: BotW ground material index
    u8 amount;              // percent
    u8 left;
    s16 pattern;
    char name[NAME_SIZE];   // the sound's name, or wav:<CRC32 of the file's first 256 bytes> when it has none
} CacheEntry;

// Sound recording (the overlay shows it): every different sound since recording started, how often and when it
// last played (system tick, the overlay turns it into "seconds ago") and what it does
typedef struct {
    char name[NAME_SIZE];
    u32 count;
    u64 last_tick;
    const CacheEntry *entry;
} MonitorEntry;

static bool g_monitor;
static MonitorEntry g_mon[MONITOR_ENTRIES];

static void MonitorNote(const CacheEntry *e) {
    u64 now = armGetSystemTick();
    MonitorEntry *oldest = &g_mon[0];
    for (u32 i = 0; i < MONITOR_ENTRIES; ++i) {
        MonitorEntry *m = &g_mon[i];
        if (m->count && strcmp(m->name, e->name) == 0) {
            ++m->count;
            m->last_tick = now;
            m->entry = e;
            return;
        }
        if (m->last_tick < oldest->last_tick) {
            oldest = m;
        }
    }
    snprintf(oldest->name, sizeof(oldest->name), "%s", e->name);
    oldest->count = 1;
    oldest->last_tick = now;
    oldest->entry = e;
}

void SoundsSetMonitor(bool on) {
    if (on && !g_monitor) {
        memset(g_mon, 0, sizeof(g_mon));
    }
    g_monitor = on;
}

void SoundsMonitorWrite(const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    // Newest first
    bool used[MONITOR_ENTRIES] = {0};
    for (u32 n = 0; n < MONITOR_ENTRIES; ++n) {
        int best = -1;
        for (u32 i = 0; i < MONITOR_ENTRIES; ++i) {
            if (!used[i] && g_mon[i].count && (best < 0 || g_mon[i].last_tick > g_mon[best].last_tick)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        const MonitorEntry *m = &g_mon[best];
        const CacheEntry *e = m->entry;
        const char *action = e->kind == Kind_Step ? "step" : e->kind == Kind_Pattern ? HapticsPatternName(e->pattern) : "-";
        fprintf(f, "%u\t%lu\t%s\t%s\n", m->count, m->last_tick, m->name, action);
    }
    fclose(f);
}

static u64 g_sap_offset, g_pool_field, g_vtable_offset;  // from the game profile or found (atkfind.c)
static u32 g_slot_size = 0x3A0, g_count = 64;
static bool g_auto, g_auto_tried, g_auto_failed, g_found_new;
static AtkLocation g_found;
static u64 g_main_base;
static u64 g_pool;
static u64 g_check_tick;
static bool g_serials_valid;
static u32 g_serials[MAX_SLOTS];
static CacheEntry g_cache[CACHE_SIZE];
static u64 g_last_play_tick[HAPTIC_PATTERN_IDS];
static u8 g_pool_buf[POOL_BUF_SIZE];
static u8 g_chunk[BARS_CHUNK + 4];
static u32 g_resolved, g_unnamed;

SoundsStatus SoundsGetStatus(void) {
    if (g_auto_failed) {
        return SoundsStatus_NotSupported;
    }
    if (g_sap_offset == 0 && !g_auto) {
        return SoundsStatus_Off;
    }
    return g_pool ? SoundsStatus_Active : SoundsStatus_Searching;
}

bool SoundsTakeFound(AtkLocation *out) {
    if (!g_found_new) {
        return false;
    }
    *out = g_found;
    g_found_new = false;
    return true;
}

void SoundsReset(void) {
    memset(g_cache, 0, sizeof(g_cache));
    g_serials_valid = false;
}

void SoundsConfigure(u64 sound_archive_player, u64 sound_pool, u64 wave_sound_vtable, u32 slot_size, u32 count, bool find) {
    g_sap_offset = sound_archive_player;
    g_pool_field = sound_pool;
    g_vtable_offset = wave_sound_vtable;
    g_slot_size = slot_size ? slot_size : 0x3A0;
    g_count = count ? count : 64;
    if ((u64)g_slot_size * g_count > POOL_BUF_SIZE || g_count > MAX_SLOTS) {
        g_count = 0;
    }
    g_auto = find && sound_archive_player == 0;
    g_auto_tried = false;
    g_auto_failed = false;
    g_pool = 0;
    g_check_tick = 0;
    SoundsReset();
}

void SoundsUpdate(const DmntCheatProcessMetadata *meta) {
    if (g_sap_offset == 0 && g_auto && !g_auto_tried) {
        g_auto_tried = true;    // once per game start: a few seconds of reading
        AtkLocation loc;
        if (AtkFind(meta, &loc) && (u64)loc.slot_size * loc.count <= POOL_BUF_SIZE && loc.count <= MAX_SLOTS) {
            g_sap_offset = loc.sound_archive_player;
            g_pool_field = loc.sound_pool;
            g_vtable_offset = loc.wave_sound_vtable;
            g_slot_size = loc.slot_size;
            g_count = loc.count;
            g_found = loc;
            g_found_new = true;
        } else {
            g_auto_failed = true;
        }
    }
    if (g_sap_offset == 0 || g_count == 0) {
        g_pool = 0;
        return;
    }
    u64 now = armGetSystemTick();
    if (now < g_check_tick && meta->main_nso_extents.base == g_main_base) {
        return;
    }
    g_check_tick = now + armNsToTicks(5000 * 1000000ULL);
    u64 base = meta->main_nso_extents.base, sap = 0, pool = 0, first = 0, last = 0;
    bool ok = R_SUCCEEDED(dmntchtReadCheatProcessMemory(base + g_sap_offset, &sap, sizeof(sap))) && sap != 0 &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(sap + g_pool_field, &pool, sizeof(pool))) && pool != 0 &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(pool, &first, sizeof(first))) &&
              R_SUCCEEDED(dmntchtReadCheatProcessMemory(pool + (g_count - 1) * g_slot_size, &last, sizeof(last))) &&
              first == base + g_vtable_offset && last == base + g_vtable_offset;
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

static bool Match(const char *mask, const char *name) {
    // '*' matches any run of characters
    if (*mask == 0) {
        return *name == 0;
    }
    if (*mask == '*') {
        for (const char *n = name;; ++n) {
            if (Match(mask + 1, n)) {
                return true;
            }
            if (*n == 0) {
                return false;
            }
        }
    }
    return *name == *mask && Match(mask + 1, name + 1);
}

static u8 MaterialByWord(const char *w) {
    static const char *Words[] = { [1] = "soil", [2] = "stone", [3] = "sand", [4] = "metal", [6] = "grass", [7] = "wood",
                                   [8] = "water", [9] = "snow", [10] = "ice", [14] = "cloth" };
    for (u32 i = 0; i < sizeof(Words) / sizeof(Words[0]); ++i) {
        if (Words[i] && strcmp(w, Words[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static void Trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = 0;
    }
    char *b = s;
    while (*b == ' ' || *b == '\t') {
        ++b;
    }
    memmove(s, b, strlen(b) + 1);
}

static void ParseRuleLine(char *line, u32 *bad) {
    char *c = strchr(line, ';');
    if (c) {
        *c = 0;
    }
    char *eq = strchr(line, '=');
    if (eq == NULL) {
        Trim(line);
        *bad += line[0] != 0;
        return;
    }
    *eq = 0;
    char *mask = line, *action = eq + 1;
    Trim(mask);
    Trim(action);
    if (mask[0] == 0 || action[0] == 0 || g_num_rules >= MAX_RULES) {
        ++*bad;
        return;
    }
    Rule r = { .amount = 100, .pattern = -1 };
    snprintf(r.mask, sizeof(r.mask), "%s", mask);
    // Optional strength at the end: "water splash 60"
    char *sp = strrchr(action, ' ');
    if (sp && sp[1] >= '0' && sp[1] <= '9') {
        int v = atoi(sp + 1);
        r.amount = v < 0 ? 0 : v > 200 ? 200 : v;
        *sp = 0;
        Trim(action);
    }
    if (strcmp(action, "ignore") == 0) {
        r.kind = Kind_Unknown;
    } else if (strncmp(action, "step", 4) == 0 && (action[4] == 0 || action[4] == ' ')) {
        r.kind = Kind_Step;
        if (action[4] == ' ') {
            r.material = MaterialByWord(action + 5);
        }
    } else {
        r.kind = Kind_Pattern;
        r.pattern = HapticsFindPattern(action);
        if (r.pattern < 0) {
            LogLine("sounds: rule \"%s\": no pattern \"%s\"", mask, action);
            ++*bad;
            return;
        }
    }
    g_rules[g_num_rules++] = r;
}

void SoundsLoadRules(const char *path, bool forget_names) {
    g_num_rules = 0;
    u32 bad = 0;
    if (path == NULL) {
        SoundsReset();
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        FILE *out = fopen(path, "w");
        if (out) {
            fputs(DefaultRules, out);
            fclose(out);
            LogLine("sounds: wrote default %s", path);
        }
        char buf[256];
        for (const char *p = DefaultRules; *p;) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            snprintf(buf, sizeof(buf), "%.*s", (int)n, p);
            ParseRuleLine(buf, &bad);
            p += n + (e != NULL);
        }
    } else {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            ParseRuleLine(line, &bad);
        }
        fclose(f);
    }
    LogLine("sounds: %u rules, %u lines not understood", g_num_rules, bad);
    if (forget_names) {
        SoundsReset();      // the rules changed: look the sounds up again
    }
}

static void Classify(const char *name, CacheEntry *e) {
    e->kind = Kind_Unknown;
    for (u32 i = 0; i < g_num_rules; ++i) {
        const Rule *r = &g_rules[i];
        if (!Match(r->mask, name)) {
            continue;
        }
        e->kind = r->kind;
        e->pattern = r->pattern;
        e->amount = r->amount;
        if (r->kind != Kind_Step) {
            return;
        }
        // Footstep: ground from "Mt_<ground>_", pace word after "Move" / "MoveShallow", heel or toe, foot
        e->material = r->material;
        const char *move = strstr(name, "Move");
        const char *pace = move ? move + 4 : name;
        if (StartsWith(pace, "Shallow")) {
            pace += 7;
        }
        if (e->material == 0 && StartsWith(name, "Mt_")) {
            const char *pl = strstr(name + 3, "_Pl_");
            e->material = MaterialFromName(name + 3, pl ? (size_t)(pl - name - 3) : strlen(name + 3));
        }
        if (e->material == 0) {
            e->material = 1;
        }
        u32 amount = PaceAmount(pace) * r->amount / 100;
        const char *part = strchr(pace, '_');
        if (part != NULL) {
            ++part;
            if (StartsWith(part, "Toe")) {
                amount = amount * 4 / 10;   // the toe follows the heel, a light second touch
            } else if (StartsWith(part, "HeelAccent")) {
                amount = amount * 8 / 10;
            }
            const char *foot = part + (StartsWith(part, "Heel") ? 4 : StartsWith(part, "Toe") ? 3 : 0);
            e->left = *foot == 'L';
        }
        e->amount = amount > 200 ? 200 : amount;
        return;
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

static void Play(bool in_game, const CacheEntry *e) {
    if (e->kind == Kind_Unknown || !in_game) {
        return;
    }
    if (e->kind == Kind_Step) {
        HapticsPlayStep(e->material, e->left, e->amount / 100.0f);
        return;
    }
    u64 now = armGetSystemTick();
    if (e->pattern < 0 || now - g_last_play_tick[e->pattern] < armNsToTicks(REPEAT_NS)) {
        return;
    }
    g_last_play_tick[e->pattern] = now;
    HapticsPlay(e->pattern, e->amount / 100.0f);
}

void SoundsPoll(bool in_game) {
    if (g_pool == 0 || !g_haptic_settings.enabled ||
        R_FAILED(dmntchtReadCheatProcessMemory(g_pool, g_pool_buf, sizeof(g_pool_buf)))) {
        g_serials_valid = false;
        return;
    }
    bool looked_up = false;     // at most one (slow) name lookup per poll
    for (u32 i = 0; i < g_count; ++i) {
        const u8 *slot = g_pool_buf + i * g_slot_size;
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
                ++g_resolved;
            } else {
                // No name (no BARS package around it): a fingerprint of the sound file, the same every start
                u8 head[0x100];
                u32 crc = R_SUCCEEDED(dmntchtReadCheatProcessMemory(fwav, head, sizeof(head))) ? crc32Calculate(head, sizeof(head)) : tag;
                snprintf(name, sizeof(name), "wav:%08X", crc);
                ++g_unnamed;
            }
            snprintf(e->name, sizeof(e->name), "%.39s", name);
            Classify(name, e);
            if (e->kind != Kind_Unknown) {
                LogVerbose("sounds: %s -> %s, material %u, %u%%", name, e->kind == Kind_Step ? "step" : HapticsPatternName(e->pattern), e->material, e->amount);
            }
        }
        Play(in_game, e);
        if (g_monitor) {
            MonitorNote(e);
        }
    }
    g_serials_valid = true;
}
