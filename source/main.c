// adaptive_triggers: watches the running application and, while BotW is running,
// attaches to it through dmnt:cht and drives the DualSense triggers through MissionControl.
//
// Skeleton stage: no game state is read yet. While BotW runs we log its module layout
// (to find the build id) and apply a light constant resistance so the lifecycle can be
// felt on the controller: on when the game starts, off when it exits.
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dmntcht.h"
#include "mc.h"
#include "botw.h"
#include "search.h"

#define BOTW_PROGRAM_ID 0x01007EF00011E000ULL

#define LOG_DIR      "sdmc:/config/adaptive_triggers"
#define LOG_PATH     LOG_DIR "/log.txt"
#define LOG_OLD_PATH LOG_DIR "/log.old.txt"
#define CONFIG_PATH  LOG_DIR "/config.ini"

#define POLL_INTERVAL_NS        (250 * 1000000ULL)
#define ACTIVE_POLL_INTERVAL_NS (50 * 1000000ULL)  // 20 Hz while attached to the game
#define ATTACH_TIMEOUT_TICKS    120 // 30 s worth of polls

// Seconds to wait after the game starts before attaching. Other tools (SaltyNX) inject
// into the game right after launch, attaching dmnt during that seems to crash the game.
static u32 g_attach_delay_s = 30;

// ---- sysmodule runtime setup ----

u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x30000 // search mode allocates ~90 KB for a 16 KB region

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

void LogLine(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (f == NULL) {
        return;
    }

    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    fprintf(f, "[%6lu.%03lu] ", ms / 1000, ms % 1000);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fclose(f);
}

static void StartLog(void) {
    mkdir(LOG_DIR, 0777);
    remove(LOG_OLD_PATH);
    rename(LOG_PATH, LOG_OLD_PATH);
    LogLine("adaptive_triggers started");
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
        } else if (sscanf(p, "shield_hit_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.shield_hit_strength = value;
        } else if (sscanf(p, "shield_hit_ms = %u", &value) == 1 && value <= 2000) {
            g_botw_settings.shield_hit_ms = value;
        } else if (sscanf(p, "shield_hit_style = %u", &value) == 1 && value >= 1 && value <= 2) {
            g_botw_settings.shield_hit_style = value;
        } else if (sscanf(p, "bow_drawn_strength = %u", &value) == 1 && value >= 1 && value <= 8) {
            g_botw_settings.bow_drawn_strength = value;
        } else if (sscanf(p, "daruk_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.daruk_strength = value;
        } else if (sscanf(p, "bow_release_strength = %u", &value) == 1 && value <= 8) {
            g_botw_settings.bow_release_strength = value;
        } else if (sscanf(p, "low_health_quarters = %u", &value) == 1 && value <= 120) {
            g_botw_settings.low_health_quarters = value;
        } else if (sscanf(p, "search = %u", &value) == 1) {
            g_search_settings.enabled = value != 0;
        } else if (sscanf(p, "search_offset = %x", &value) == 1 && value < 0x100000) {
            g_search_settings.offset = value & ~3u;
        } else if (sscanf(p, "search_size = %x", &value) == 1 && value >= 0x100 && value <= 0x4000) {
            g_search_settings.size = value & ~3u;
        } else if (strncmp(p, "watch=", 6) == 0) {
            ParseWatch(p + 6);
        }
    }
    fclose(f);

    LogLine("config: attach_delay_s=%u, shield_hit_strength=%u, shield_hit_ms=%u, low_health_quarters=%u, %u watches",
            g_attach_delay_s, g_botw_settings.shield_hit_strength, g_botw_settings.shield_hit_ms, g_botw_settings.low_health_quarters, g_num_watches);
    if (g_search_settings.enabled) {
        LogLine("config: search on, region +%X size %X", g_search_settings.offset, g_search_settings.size);
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
} GameSession;

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
    if (s->meta.process_id != s->pid || s->meta.program_id != BOTW_PROGRAM_ID) {
        LogLine("dmnt attached to another process (pid %lu, program %016lX), giving up", s->meta.process_id, s->meta.program_id);
        s->state = SessionState_Failed;
        CloseSession(s);
        return;
    }

    LogLine("attached to BotW after %u tries", s->attach_tries + 1);
    LogMetadata(&s->meta);
    CheckMemoryRead(&s->meta);
    BotwInit(&s->botw, &s->meta);

    if (R_SUCCEEDED(rc = mcInitialize())) {
        s->mc_open = true;

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
        LogLine("could not attach to BotW, giving up until it restarts");
        s->state = SessionState_Failed;
        CloseSession(s);
    }
}

static void Update(GameSession *s) {
    u64 pid = 0, program_id = 0;
    bool botw_running = GetCurrentApplication(&pid, &program_id) && program_id == BOTW_PROGRAM_ID;

    // Game exited or was restarted
    if (s->state != SessionState_Idle && (!botw_running || pid != s->pid)) {
        LogLine("BotW (pid %lu) is gone, releasing", s->pid);
        CloseSession(s);
        memset(s, 0, sizeof(*s));
    }

    if (s->state == SessionState_Idle && botw_running) {
        LogLine("BotW started, pid %lu", pid);
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
        BotwUpdate(&s->botw, &s->meta);

        // Send effects only when they change, the controller keeps the last ones
        TriggerEffects effects;
        if (s->mc_open && BotwComputeEffects(&s->botw, &effects) &&
            (!s->sent_valid || memcmp(&effects, &s->sent, sizeof(effects)) != 0)) {
            Result rc = mcSetTriggerEffects(&effects);
            if (R_SUCCEEDED(rc)) {
                LogLine("effects R2 mode %02X, L2 mode %02X", effects.right.mode, effects.left.mode);
                s->sent = effects;
                s->sent_valid = true;
            } else {
                LogLine("SetTriggerEffects failed: 0x%X", rc);
            }
        }

        // Lightbar: heartbeat at low health, player colour otherwise
        u8 rgb[3];
        if (s->mc_open) {
            bool want = BotwComputeLightbar(&s->botw, &rgb[0], &rgb[1], &rgb[2]);
            if (want && (!s->lightbar_set || memcmp(rgb, s->lightbar, 3) != 0)) {
                if (R_SUCCEEDED(mcSetLightbarColour(rgb[0], rgb[1], rgb[2]))) {
                    if (!s->lightbar_set) {
                        LogLine("lightbar: low health heartbeat on");
                    }
                    memcpy(s->lightbar, rgb, 3);
                    s->lightbar_set = true;
                }
            } else if (!want && s->lightbar_set) {
                mcClearLightbarColour();
                s->lightbar_set = false;
                LogLine("lightbar: back to player colour");
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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (R_FAILED(g_init_rc)) {
        return 0;
    }

    StartLog();
    LoadConfig();

    GameSession session = {0};
    while (true) {
        Update(&session);
        svcSleepThread(session.state == SessionState_Active ? ACTIVE_POLL_INTERVAL_NS : POLL_INTERVAL_NS);
    }

    return 0;
}
