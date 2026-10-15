// DualsenseFX sysmodule: watches the running application and, when the game has a profile
// (config/dualsensefx/games/<program id>/game.ini), attaches to it through dmnt:cht (read only) and drives the
// DualSense (MissionControl) or the Joy-Con / Pro Controller rumble from the game's state and sounds.
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dmntcht.h"
#include "mc.h"
#include "botw.h"
#include "haptics.h"
#include "dump.h"
#include "net.h"
#include "search.h"
#include "sounds.h"
#include "profile.h"

#define LOG_DIR      DFX_DIR
#define LOG_PATH     LOG_DIR "/log.txt"
#define LOG_OLD_PATH LOG_DIR "/log.old.txt"
#define CONFIG_PATH  LOG_DIR "/config.ini"

// The current game's profile files (none while no game with a profile runs)
static char g_patterns_path[128];
static char g_sounds_path[128];

#define POLL_INTERVAL_NS        (250 * 1000000ULL)
#define ACTIVE_POLL_INTERVAL_NS (50 * 1000000ULL)  // 20 Hz while attached to the game
#define FAST_POLL_INTERVAL_NS   (10 * 1000000ULL)  // one-frame events (BotwPollFast)
#define ATTACH_TIMEOUT_TICKS    120 // 30 s worth of polls

// Seconds to wait after the game starts before attaching. Other tools (SaltyNX) inject
// into the game right after launch, attaching dmnt during that seems to crash the game.
static u32 g_attach_delay_s = 30;

// ---- sysmodule runtime setup ----

u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0xA0000 // search modes up to ~350 KB, network transfer memory ~170 KB

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Set when a service failed to start. The module then exits instead of aborting:
// an abort in a boot2 sysmodule would show a fatal error on every boot.
static Result g_init_rc = 0;

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        g_init_rc = rc;
        return;
    }

    // libnx checks the firmware version before using some commands
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    if (R_FAILED(rc = fsInitialize()) || R_FAILED(rc = fsdevMountSdmc()) ||
        R_FAILED(rc = pmdmntInitialize()) || R_FAILED(rc = pminfoInitialize())) {
        g_init_rc = rc;
    }
    // sm stays open: dmnt:cht and mc are connected later, only while BotW is running
}

void __appExit(void) {
    pminfoExit();
    pmdmntExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---- logging ----

static bool g_verbose = false;

static void LogWrite(const char *fmt, va_list args) {
    FILE *f = fopen(LOG_PATH, "a");
    if (f == NULL) {
        return;
    }

    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    fprintf(f, "[%6lu.%03lu] ", ms / 1000, ms % 1000);
    vfprintf(f, fmt, args);
    fputc('\n', f);
    fclose(f);
}

void LogLine(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogWrite(fmt, args);
    va_end(args);
}

// Per-event gameplay logging (effect changes, item values...), only with verbose=1 in config.ini
void LogVerbose(const char *fmt, ...) {
    if (!g_verbose) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    LogWrite(fmt, args);
    va_end(args);
}


static void StartLog(void) {
    mkdir(LOG_DIR, 0777);
    remove(LOG_OLD_PATH);
    rename(LOG_PATH, LOG_OLD_PATH);
    LogLine("DualsenseFX started");
}

// ---- watches: pointer chains from config.ini whose values are logged when they change ----

#define MAX_WATCHES       16
#define MAX_WATCH_OFFSETS 8
#define LOG_MAX_BYTES     (2 * 1024 * 1024)

typedef enum {
    WatchType_U8, WatchType_U16, WatchType_U32, WatchType_S32, WatchType_U64, WatchType_F32,
} WatchType;

typedef enum {
    WatchBase_Main, WatchBase_Heap,
} WatchBase;

typedef struct {
    char name[24];
    WatchBase base;
    u32 num_offsets;
    u64 offsets[MAX_WATCH_OFFSETS]; // first is added to the base, each next one is added after a pointer dereference
    WatchType type;
    bool valid;                     // last read succeeded
    u64 last_raw;
} Watch;

static Watch g_watches[MAX_WATCHES];
static u32 g_num_watches;

static const struct { const char *name; WatchType type; u32 size; } WatchTypes[] = {
    { "u8",  WatchType_U8,  1 }, { "u16", WatchType_U16, 2 }, { "u32", WatchType_U32, 4 },
    { "s32", WatchType_S32, 4 }, { "u64", WatchType_U64, 8 }, { "f32", WatchType_F32, 4 },
};

// "watch=name main+2CA6D48 447D8 20 s32": addr = main+2CA6D48, then addr = [addr]+447D8, then addr = [addr]+20, read s32
// (same semantics as an Atmosphère cheat pointer chain)
static void ParseWatch(char *spec) {
    if (g_num_watches >= MAX_WATCHES) {
        return;
    }

    Watch w = {0};
    char *save = NULL;
    char *tok = strtok_r(spec, " \t\r\n", &save);
    if (tok == NULL) {
        return;
    }
    snprintf(w.name, sizeof(w.name), "%s", tok);

    bool have_type = false;
    while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL && !have_type) {
        if (w.num_offsets == 0) {
            if (strncmp(tok, "main+", 5) == 0)      { w.base = WatchBase_Main; tok += 5; }
            else if (strncmp(tok, "heap+", 5) == 0) { w.base = WatchBase_Heap; tok += 5; }
            else { LogLine("watch %s: base must be main+ or heap+", w.name); return; }
        } else {
            for (size_t i = 0; i < sizeof(WatchTypes) / sizeof(WatchTypes[0]); ++i) {
                if (strcmp(tok, WatchTypes[i].name) == 0) {
                    w.type = WatchTypes[i].type;
                    have_type = true;
                }
            }
            if (have_type) {
                break;
            }
        }
        if (w.num_offsets >= MAX_WATCH_OFFSETS) {
            LogLine("watch %s: too many offsets", w.name);
            return;
        }
        w.offsets[w.num_offsets++] = strtoull(tok, NULL, 16);
    }

    if (!have_type || w.num_offsets == 0) {
        LogLine("watch %s: expected \"name main+OFF [OFF...] type\"", w.name);
        return;
    }

    g_watches[g_num_watches++] = w;
}

static Result ResolveWatch(const Watch *w, const DmntCheatProcessMetadata *meta, u64 *out_addr) {
    u64 addr = (w->base == WatchBase_Main ? meta->main_nso_extents.base : meta->heap_extents.base) + w->offsets[0];
    for (u32 i = 1; i < w->num_offsets; ++i) {
        u64 ptr = 0;
        Result rc = dmntchtReadCheatProcessMemory(addr, &ptr, sizeof(ptr));
        if (R_FAILED(rc)) {
            return rc;
        }
        if (ptr == 0) {
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        }
        addr = ptr + w->offsets[i];
    }
    *out_addr = addr;
    return 0;
}

static void FormatWatchValue(const Watch *w, u64 raw, char *out, size_t size) {
    switch (w->type) {
        case WatchType_S32: snprintf(out, size, "%d", (s32)(u32)raw); break;
        case WatchType_F32: { float f; u32 v = (u32)raw; memcpy(&f, &v, sizeof(f)); snprintf(out, size, "%.4f", f); break; }
        case WatchType_U64: snprintf(out, size, "0x%lX", raw); break;
        default:            snprintf(out, size, "%lu (0x%lX)", raw, raw); break;
    }
}

static bool LogTooBig(void) {
    struct stat st;
    return stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES;
}

static void UpdateWatches(const DmntCheatProcessMetadata *meta) {
    for (u32 i = 0; i < g_num_watches; ++i) {
        Watch *w = &g_watches[i];

        u64 addr = 0, raw = 0;
        Result rc = ResolveWatch(w, meta, &addr);
        if (R_SUCCEEDED(rc)) {
            rc = dmntchtReadCheatProcessMemory(addr, &raw, WatchTypes[w->type].size);
        }

        if (R_FAILED(rc)) {
            if (w->valid) {
                LogLine("watch %s: unreadable (0x%X)", w->name, rc);
                w->valid = false;
            }
            continue;
        }

        if (!w->valid || raw != w->last_raw) {
            if (!LogTooBig()) {
                char value[48];
                FormatWatchValue(w, raw, value, sizeof(value));
                LogLine("watch %s = %s @%010lX", w->name, value, addr);
            }
            w->valid = true;
            w->last_raw = raw;
        }
    }
}

// ---- config ----

// config.ini, one "key=value" per line, ';' starts a comment
static void LoadConfig(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f == NULL) {
        LogLine("no config.ini, attach_delay_s=%u", g_attach_delay_s);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned value;
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == ';') {
            continue;
        }
        if (sscanf(p, "attach_delay_s = %u", &value) == 1 && value <= 600) {
            g_attach_delay_s = value;
        } else if (sscanf(p, "verbose = %u", &value) == 1) {
            g_verbose = value != 0;
        } else if (sscanf(p, "shield_hit_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.shield_hit_strength = value;
        } else if (sscanf(p, "shield_hit_ms = %u", &value) == 1 && value <= 2000) {
            g_botw_settings.shield_hit_ms = value;
        } else if (sscanf(p, "shield_hit_style = %u", &value) == 1 && value >= 1 && value <= 2) {
            g_botw_settings.shield_hit_style = value;
        } else if (sscanf(p, "bow_drawn_strength = %u", &value) == 1 && value >= 1 && value <= 8) {
            g_botw_settings.bow_drawn_strength = value;
        } else if (sscanf(p, "surf_rumble_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.surf_rumble_strength = value;
        } else if (sscanf(p, "surf_timeout_ms = %u", &value) == 1 && value >= 500 && value <= 60000) {
            g_botw_settings.surf_timeout_ms = value;
        } else if (sscanf(p, "surf_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.surf_strength = value;
        } else if (sscanf(p, "daruk_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.daruk_strength = value;
        } else if (sscanf(p, "bow_release_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.bow_release_strength = value;
        } else if (sscanf(p, "net = %u", &value) == 1) {
            g_net_settings.enabled = value != 0;
        } else if (sscanf(p, "net_port = %u", &value) == 1 && value >= 1024 && value < 65535) {
            g_net_settings.port = value;
        } else if (strncmp(p, "net_token=", 10) == 0) {
            char *v = p + 10;
            v[strcspn(v, "\r\n \t")] = 0;
            snprintf(g_net_settings.token, sizeof(g_net_settings.token), "%s", v);
        } else if (sscanf(p, "dump_main = %u", &value) == 1) {
            g_dump_main = value != 0;
        } else if (sscanf(p, "haptics = %u", &value) == 1) {
            g_haptic_settings.enabled = value != 0;
        } else if (sscanf(p, "haptics_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.strength = value;
        } else if (sscanf(p, "footstep_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.group[HapticGroup_Footsteps] = value;
        } else if (sscanf(p, "combat_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.group[HapticGroup_Combat] = value;
        } else if (sscanf(p, "body_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.group[HapticGroup_Body] = value;
        } else if (sscanf(p, "rune_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.group[HapticGroup_Runes] = value;
        } else if (sscanf(p, "world_strength = %u", &value) == 1 && value <= 200) {
            g_haptic_settings.group[HapticGroup_World] = value;
        } else if (sscanf(p, "output = %u", &value) == 1 && value <= 2) {
            g_haptic_settings.output = value;
        } else if (sscanf(p, "triggers = %u", &value) == 1) {
            g_botw_settings.triggers = value != 0;
        } else if (sscanf(p, "low_health_quarters = %u", &value) == 1 && value <= 120) {
            g_botw_settings.low_health_quarters = value;
        } else if (sscanf(p, "search = %u", &value) == 1) {
            g_search_settings.enabled = value != 0;
            if (value >= 1 && value <= 4) {
                g_search_settings.mode = (SearchMode)value;
            }
        } else if (strncmp(p, "search_base=rune", 16) == 0) {
            g_search_settings.base_rune = true;
        } else if (sscanf(p, "search_offset = %x", &value) == 1 && value < 0x100000) {
            g_search_settings.offset = value & ~3u;
        } else if (sscanf(p, "search_size = %x", &value) == 1 && value >= 0x100 && value <= 0x8000) {
            g_search_settings.size = value & ~3u;
        } else if (strncmp(p, "watch=", 6) == 0) {
            ParseWatch(p + 6);
        }
    }
    fclose(f);

    LogLine("config: verbose=%u", g_verbose);
    LogLine("config: attach_delay_s=%u, shield_hit_strength=%u, shield_hit_ms=%u, low_health_quarters=%u, %u watches",
            g_attach_delay_s, g_botw_settings.shield_hit_strength, g_botw_settings.shield_hit_ms, g_botw_settings.low_health_quarters, g_num_watches);
    LogLine("config: output=%u", g_haptic_settings.output);
    LogLine("config: triggers=%u, haptics=%u, haptics_strength=%u, combat %u, body %u, runes %u, world %u, footsteps %u",
            g_botw_settings.triggers, g_haptic_settings.enabled, g_haptic_settings.strength,
            g_haptic_settings.group[HapticGroup_Combat], g_haptic_settings.group[HapticGroup_Body], g_haptic_settings.group[HapticGroup_Runes],
            g_haptic_settings.group[HapticGroup_World], g_haptic_settings.group[HapticGroup_Footsteps]);
    LogLine("config: net=%u, net_port=%u, token %s", g_net_settings.enabled, g_net_settings.port, g_net_settings.token[0] ? "set" : "empty");
    if (g_search_settings.enabled) {
        LogLine("config: search mode %u, region +%X size %X", g_search_settings.mode, g_search_settings.offset, g_search_settings.size);
    }
    for (u32 i = 0; i < g_num_watches; ++i) {
        const Watch *w = &g_watches[i];
        char chain[128] = {0};
        size_t len = 0;
        for (u32 j = 0; j < w->num_offsets && len < sizeof(chain); ++j) {
            if (j == 0) {
                len += snprintf(chain + len, sizeof(chain) - len, "%s+%lX", w->base == WatchBase_Main ? "main" : "heap", w->offsets[j]);
            } else {
                len += snprintf(chain + len, sizeof(chain) - len, " %lX", w->offsets[j]);
            }
        }
        LogLine("  watch %s: %s %s", w->name, chain, WatchTypes[w->type].name);
    }
}

void ReloadConfig(void) {
    g_num_watches = 0;      // they are read again
    LoadConfig();
    HapticsApplyOutput();
}

// The overlay (or anything else) writes the files, the module picks the change up within a second. The content is
// compared (CRC32): FAT timestamps have 2 s steps and a changed value often keeps the file size.
typedef struct {
    const char *path;
    u32 crc;
    bool have;
} WatchedFile;

static bool FileChanged(WatchedFile *w) {
    static char buf[32768];
    FILE *f = fopen(w->path, "r");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    u32 crc = crc32Calculate(buf, n);
    bool changed = w->have && crc != w->crc;
    w->crc = crc;
    w->have = true;
    return changed;
}

// Rules refer to patterns by name, so they are read after them. A strength change in patterns.ini keeps the looked-up
// sound names (pattern ids stay the same); a change in sounds.ini looks them up again.
static bool g_profile_builtin;  // the current game is BotW (built-in patterns)

static void LoadPatternsAndRules(bool forget_names) {
    HapticsLoadPatterns(DFX_DIR "/patterns.ini", g_patterns_path[0] ? g_patterns_path : NULL, g_profile_builtin);
    SoundsLoadRules(g_sounds_path[0] ? g_sounds_path : NULL, forget_names);
}

// Sound recording: the overlay creates monitor.on to start it and removes it to stop; it keeps running while the
// overlay is closed
static void UpdateMonitor(void) {
    static u64 next_check, next_write;
    static bool on;
    u64 now = armGetSystemTick();
    if (now >= next_check) {
        next_check = now + armNsToTicks(1000000000ULL);
        struct stat st;
        bool want = stat(DFX_DIR "/monitor.on", &st) == 0;
        if (want != on) {
            on = want;
            SoundsSetMonitor(on);
            LogLine("sound monitor %s", on ? "on" : "off");
        }
    }
    if (on && now >= next_write) {
        next_write = now + armNsToTicks(1000000000ULL);
        SoundsMonitorWrite(DFX_DIR "/monitor.txt");
    }
}

static void CheckConfigChanged(void) {
    static u64 next_tick;
    static WatchedFile config = { CONFIG_PATH, 0, false }, patterns = { g_patterns_path, 0, false }, sounds = { g_sounds_path, 0, false };
    static WatchedFile library = { DFX_DIR "/patterns.ini", 0, false };
    u64 now = armGetSystemTick();
    if (now < next_tick) {
        return;
    }
    next_tick = now + armNsToTicks(1000000000ULL);
    if (FileChanged(&config)) {
        LogLine("config.ini changed, reloading");
        ReloadConfig();
    }
    bool lib = FileChanged(&library);
    if (g_patterns_path[0] == 0) {
        patterns.have = sounds.have = false;    // no game: start fresh with the next one
        if (lib) {
            LoadPatternsAndRules(false);
        }
        return;
    }
    bool p = FileChanged(&patterns) || lib, r = FileChanged(&sounds);
    if (p || r) {
        LogLine("%s changed, reloading", p ? "patterns.ini" : "sounds.ini");
        LoadPatternsAndRules(r);
    }
}

// ---- game session ----

typedef enum {
    SessionState_Idle,      // BotW not running
    SessionState_Attaching, // BotW running, waiting until dmnt can attach
    SessionState_Active,    // attached, effects applied
    SessionState_Failed,    // gave up on this process, wait for it to exit
} SessionState;

typedef struct {
    SessionState state;
    u64 pid;
    u64 start_tick;
    u32 attach_tries;
    Result last_attach_rc;
    bool dmnt_open;
    bool mc_open;
    DmntCheatProcessMetadata meta;
    BotwState botw;
    TriggerEffects sent;
    bool sent_valid;
    bool lightbar_set;
    u8 lightbar[3];
    bool search_started;
    GameProfile profile;
} GameSession;

static u64 g_no_profile_pid;   // the running application has no (enabled) profile: look again every few seconds
static u64 g_no_profile_recheck;

static bool GetCurrentApplication(u64 *out_pid, u64 *out_program_id) {
    if (R_FAILED(pmdmntGetApplicationProcessId(out_pid))) {
        return false;
    }
    return R_SUCCEEDED(pminfoGetProgramId(out_program_id, *out_pid));
}

static void LogMetadata(const DmntCheatProcessMetadata *meta) {
    char build_id[2 * 0x20 + 1];
    for (int i = 0; i < 0x20; ++i) {
        snprintf(&build_id[2 * i], 3, "%02X", meta->main_nso_build_id[i]);
    }

    LogLine("  pid        %lu", meta->process_id);
    LogLine("  program_id %016lX", meta->program_id);
    LogLine("  build_id   %s", build_id);
    LogLine("  main       base %010lX size %08lX", meta->main_nso_extents.base, meta->main_nso_extents.size);
    LogLine("  heap       base %010lX size %08lX", meta->heap_extents.base, meta->heap_extents.size);
    LogLine("  alias      base %010lX size %08lX", meta->alias_extents.base, meta->alias_extents.size);
    LogLine("  aslr       base %010lX size %08lX", meta->aslr_extents.base, meta->aslr_extents.size);
}

// Sanity check that memory reads work: the main module's header points to a "MOD0" block
static void CheckMemoryRead(const DmntCheatProcessMetadata *meta) {
    u32 mod0_offset = 0;
    Result rc = dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + 4, &mod0_offset, sizeof(mod0_offset));
    if (R_FAILED(rc)) {
        LogLine("  read main+4 failed: 0x%X", rc);
        return;
    }

    char magic[5] = {0};
    rc = dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + mod0_offset, magic, 4);
    if (R_FAILED(rc)) {
        LogLine("  read MOD0 at main+%X failed: 0x%X", mod0_offset, rc);
        return;
    }

    LogLine("  main+%X reads \"%s\" (expected \"MOD0\")", mod0_offset, magic);
}

static void CloseSession(GameSession *s) {
    SearchStop();
    DumpStop();
    HapticsSetAvailable(false);
    HapticsSetMcOpen(false);
    if (s->mc_open) {
        mcExit();
        s->mc_open = false;
    }
    if (s->dmnt_open) {
        dmntchtExit();
        s->dmnt_open = false;
    }
}

static void TryAttach(GameSession *s) {
    Result rc = 0;

    if (!s->dmnt_open) {
        if (R_FAILED(rc = dmntchtInitialize())) {
            goto fail;
        }
        s->dmnt_open = true;
    }

    // dmnt only auto-attaches to games that have a cheat file, so attach explicitly
    if (R_FAILED(rc = dmntchtForceOpenCheatProcess())) {
        goto fail;
    }
    if (R_FAILED(rc = dmntchtGetCheatProcessMetadata(&s->meta))) {
        goto fail;
    }
    if (s->meta.process_id != s->pid || s->meta.program_id != s->profile.program_id) {
        LogLine("dmnt attached to another process (pid %lu, program %016lX), giving up", s->meta.process_id, s->meta.program_id);
        s->state = SessionState_Failed;
        CloseSession(s);
        return;
    }

    LogLine("attached to %s after %u tries", s->profile.name, s->attach_tries + 1);
    LogMetadata(&s->meta);
    if (!ProfileBuildMatches(&s->profile, s->meta.main_nso_build_id)) {
        LogLine("this build is not in the profile's builds, leaving the game alone");
        s->state = SessionState_Failed;
        CloseSession(s);
        return;
    }
    CheckMemoryRead(&s->meta);
    // The built-in BotW logic (inventory, bow, runes, surfing) checks the build itself
    if (s->profile.program_id == BOTW_PROGRAM_ID) {
        BotwInit(&s->botw, &s->meta);
    }
    SoundsConfigure(s->profile.sound_archive_player, s->profile.sound_pool, s->profile.wave_sound_vtable,
                    s->profile.sound_slot_size, s->profile.sound_slots, s->profile.sound_find);
    ProfileSetCurrent(&s->profile);
    DumpStart(&s->meta);

    HapticsSetAvailable(true);       // Joy-Con output works without MissionControl
    if (R_SUCCEEDED(rc = mcInitialize())) {
        s->mc_open = true;
        HapticsSetMcOpen(true);

        char version[64];
        mcGetBuildVersionString(version, sizeof(version));
        LogLine("mc build %s", version);

    } else {
        LogLine("mc service unavailable: 0x%X", rc);
    }

    s->state = SessionState_Active;
    return;

fail:
    // Early in the game's boot dmnt can't attach yet, retry quietly and log only changes
    if (rc != s->last_attach_rc) {
        LogLine("attach attempt %u: 0x%X", s->attach_tries + 1, rc);
        s->last_attach_rc = rc;
    }
    if (++s->attach_tries >= ATTACH_TIMEOUT_TICKS) {
        LogLine("could not attach to %s, giving up until it restarts", s->profile.name);
        s->state = SessionState_Failed;
        CloseSession(s);
    }
}

static void Update(GameSession *s) {
    u64 pid = 0, program_id = 0;
    bool running = GetCurrentApplication(&pid, &program_id);

    // Game exited or was restarted
    if (!running && g_no_profile_pid != 0) {
        g_no_profile_pid = 0;
        ProfileSetCurrent(NULL);
    }
    if (s->state != SessionState_Idle && (!running || pid != s->pid)) {
        LogLine("%s (pid %lu) is gone, releasing", s->profile.name, s->pid);
        CloseSession(s);
        memset(s, 0, sizeof(*s));
        ProfileSetCurrent(NULL);
        SoundsConfigure(0, 0, 0, 0, 0, false);
        g_patterns_path[0] = g_sounds_path[0] = 0;
        LoadPatternsAndRules(true);
    }

    if (s->state == SessionState_Idle && running && (pid != g_no_profile_pid || armGetSystemTick() >= g_no_profile_recheck)) {
        if (!ProfileLoad(program_id, &s->profile) || !s->profile.enabled) {
            if (pid != g_no_profile_pid) {
                LogLine("application %016lX has %s, not attaching", program_id, s->profile.name[0] ? "its profile turned off" : "no profile");
                ProfileSetCurrentWithout(program_id);    // the overlay offers to create one
            }
            g_no_profile_pid = pid;
            g_no_profile_recheck = armGetSystemTick() + armNsToTicks(5000000000ULL);
            return;
        }
        LogLine("%s started, pid %lu", s->profile.name, pid);
        ProfilePath(&s->profile, "patterns.ini", g_patterns_path, sizeof(g_patterns_path));
        g_profile_builtin = s->profile.program_id == BOTW_PROGRAM_ID;
        ProfilePath(&s->profile, "sounds.ini", g_sounds_path, sizeof(g_sounds_path));
        LoadPatternsAndRules(true);
        s->state = SessionState_Attaching;
        s->pid = pid;
        s->start_tick = armGetSystemTick();
        for (u32 i = 0; i < g_num_watches; ++i) {
            g_watches[i].valid = false;
        }
        LogLine("waiting %u s before attaching", g_attach_delay_s);
    }

    if (s->state == SessionState_Active) {
        UpdateWatches(&s->meta);
        DumpUpdate();
        BotwUpdate(&s->botw, &s->meta);
        SoundsUpdate(&s->meta);
        AtkLocation found;
        if (SoundsTakeFound(&found)) {
            ProfileSaveSoundPool(&s->profile, found.sound_archive_player, found.sound_pool, found.wave_sound_vtable,
                                 found.slot_size, found.count);
        }

        // Send effects only when they change, the controller keeps the last ones
        TriggerEffects effects;
        // Triggers and lightbar exist on the DualSense only
        bool dualsense = s->mc_open && g_haptic_settings.output == HapticOutput_DualSense;
        bool have_effects = BotwComputeEffects(&s->botw, &effects);
        if (!dualsense) {
            effects.right = TriggerEffectOff();
            effects.left = TriggerEffectOff();
        }
        bool net_override = NetTriggerOverride(&effects);
        have_effects = s->mc_open && (have_effects || net_override);
        if (have_effects && (!s->sent_valid || memcmp(&effects, &s->sent, sizeof(effects)) != 0)) {
            Result rc = mcSetTriggerEffects(&effects);
            if (R_SUCCEEDED(rc)) {
                LogVerbose("effects R2 mode %02X, L2 mode %02X", effects.right.mode, effects.left.mode);
                s->sent = effects;
                s->sent_valid = true;
            } else {
                LogLine("SetTriggerEffects failed: 0x%X", rc);
            }
        }

        // Lightbar: heartbeat at low health, player colour otherwise
        u8 rgb[3];
        if (s->mc_open) {
            bool want = BotwComputeLightbar(&s->botw, &rgb[0], &rgb[1], &rgb[2]) && dualsense;
            want |= NetLightbarOverride(&rgb[0], &rgb[1], &rgb[2]);
            if (want && (!s->lightbar_set || memcmp(rgb, s->lightbar, 3) != 0)) {
                if (R_SUCCEEDED(mcSetLightbarColour(rgb[0], rgb[1], rgb[2]))) {
                    if (!s->lightbar_set) {
                        LogVerbose("lightbar: override on (heartbeat or flash)");
                    }
                    memcpy(s->lightbar, rgb, 3);
                    s->lightbar_set = true;
                }
            } else if (!want && s->lightbar_set) {
                mcClearLightbarColour();
                s->lightbar_set = false;
                LogVerbose("lightbar: back to player colour");
            }
        }

        if (s->mc_open) {
            if (!s->search_started) {
                s->search_started = SearchStart(&s->meta);
            }
            SearchUpdate(&s->meta);
        }
    }

    if (s->state == SessionState_Attaching) {
        u64 running_ns = armTicksToNs(armGetSystemTick() - s->start_tick);
        if (running_ns >= g_attach_delay_s * 1000000000ULL) {
            TryAttach(s);
        }
    }
}

// MissionControl HD haptics diagnostics, logged when they change (opens a short mc session when no game runs)
static void LogHapticsStatus(bool mc_open) {
    static McHapticsStatus last;
    static bool have_last;

    if (!mc_open && R_FAILED(mcInitialize())) {
        return;
    }
    McHapticsStatus st;
    Result rc = mcGetHapticsStatus(&st);
    if (!mc_open) {
        mcExit();
    }
    if (R_FAILED(rc)) {
        return;
    }

    if (!have_last || st.enabled != last.enabled || st.thread_running != last.thread_running ||
        st.reports_sent != last.reports_sent || st.reports_failed != last.reports_failed ||
        st.chirps != last.chirps || st.fallbacks != last.fallbacks ||
        st.input_reports != last.input_reports || st.input_other != last.input_other || st.voices != last.voices) {
        LogLine("haptics: enabled %u, thread %u, ticks %u, sent %u, failed %u, last error 0x%X, "
                "prime failed 0x%X, chirps %u, fallbacks %u",
                st.enabled, st.thread_running, st.ticks, st.reports_sent, st.reports_failed, st.last_error,
                st.prime_failed, st.chirps, st.fallbacks);
        LogLine("haptics input: 0x31 %u, mic %u, other %u, last %02X %02X %02X %02X %02X %02X %02X %02X",
                st.input_reports, st.input_mic, st.input_other,
                st.last_input[0], st.last_input[1], st.last_input[2], st.last_input[3],
                st.last_input[4], st.last_input[5], st.last_input[6], st.last_input[7]);
        LogLine("haptics input phases: connect %u, pause %u, test buzz %u, after %u; pattern voices %u",
                st.input_phase[0], st.input_phase[1], st.input_phase[2], st.input_phase[3], st.voices);
        last = st;
        have_last = true;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (R_FAILED(g_init_rc)) {
        return 0;
    }

    StartLog();
    ProfileInstallDefaults();
    LoadConfig();
    HapticsApplyOutput();
    LoadPatternsAndRules(true);

    GameSession session = {0};
    u64 next_haptics_log = armGetSystemTick() + armNsToTicks(20000000000ULL);
    u64 next_update = 0;
    while (true) {
        // Game logic keeps its 20 Hz (4 Hz idle) rhythm; the network is served in between, often while a client is connected
        u64 now = armGetSystemTick();
        if (now >= next_update) {
            Update(&session);
            next_update = now + armNsToTicks(session.state == SessionState_Active ? ACTIVE_POLL_INTERVAL_NS : POLL_INTERVAL_NS);

            if (armGetSystemTick() >= next_haptics_log) {
                next_haptics_log = armGetSystemTick() + armNsToTicks(10000000000ULL);
                if (session.state == SessionState_Idle || session.mc_open) {
                    LogHapticsStatus(session.mc_open);
                }
            }
        }

        NetContext net = {
            .attached = session.state == SessionState_Active,
            .mc_open = session.mc_open,
            .meta = &session.meta,
            .botw = &session.botw,
        };
        NetUpdate(&net);
        CheckConfigChanged();
        UpdateMonitor();
        if (session.state == SessionState_Active) {
            BotwPollFast(&session.botw);
            BotwState *b = &session.botw;
            SoundsPoll(b->supported ? BotwInGame(b) && !b->menu_open : true);
        }

        now = armGetSystemTick();
        u64 sleep_ticks = next_update > now ? next_update - now : 0;
        if (session.state == SessionState_Active && sleep_ticks > armNsToTicks(FAST_POLL_INTERVAL_NS)) {
            sleep_ticks = armNsToTicks(FAST_POLL_INTERVAL_NS);
        }
        if (NetClientConnected() && sleep_ticks > armNsToTicks(5000000ULL)) {
            sleep_ticks = armNsToTicks(5000000ULL);
        } else if (g_net_settings.enabled && sleep_ticks > armNsToTicks(50000000ULL)) {
            sleep_ticks = armNsToTicks(50000000ULL);
        }
        svcSleepThread(armTicksToNs(sleep_ticks));
    }

    return 0;
}
