// adaptive_triggers: watches the running application and, while BotW is running,
// attaches to it through dmnt:cht and drives the DualSense triggers through MissionControl.
//
// Skeleton stage: no game state is read yet. While BotW runs we log its module layout
// (to find the build id) and apply a light constant resistance so the lifecycle can be
// felt on the controller: on when the game starts, off when it exits.
#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "dmntcht.h"
#include "mc.h"

#define BOTW_PROGRAM_ID 0x01007EF00011E000ULL

#define LOG_DIR      "sdmc:/config/adaptive_triggers"
#define LOG_PATH     LOG_DIR "/log.txt"
#define LOG_OLD_PATH LOG_DIR "/log.old.txt"
#define CONFIG_PATH  LOG_DIR "/config.ini"

#define POLL_INTERVAL_NS     (250 * 1000000ULL)
#define ATTACH_TIMEOUT_TICKS 120 // 30 s worth of polls

// Seconds to wait after the game starts before attaching. Other tools (SaltyNX) inject
// into the game right after launch, attaching dmnt during that seems to crash the game.
static u32 g_attach_delay_s = 30;

// ---- sysmodule runtime setup ----

u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x20000

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

static void LogLine(const char *fmt, ...) {
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

// ---- config ----

// config.ini, one "key=value" per line, ';' starts a comment
static void LoadConfig(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f == NULL) {
        LogLine("no config.ini, attach_delay_s=%u", g_attach_delay_s);
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned value;
        if (sscanf(line, " attach_delay_s = %u", &value) == 1 && value <= 600) {
            g_attach_delay_s = value;
        }
    }
    fclose(f);

    LogLine("config: attach_delay_s=%u", g_attach_delay_s);
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

    if (R_SUCCEEDED(rc = mcInitialize())) {
        s->mc_open = true;

        char version[64];
        mcGetBuildVersionString(version, sizeof(version));
        LogLine("mc build %s", version);

        // Skeleton: light constant resistance on both triggers while the game runs
        TriggerEffects effects = {
            .right = TriggerEffectSimpleFeedback(0x30, 0x80),
            .left  = TriggerEffectSimpleFeedback(0x30, 0x80),
        };
        rc = mcSetTriggerEffects(&effects);
        LogLine("SetTriggerEffects: 0x%X", rc);
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
        LogLine("waiting %u s before attaching", g_attach_delay_s);
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
        svcSleepThread(POLL_INTERVAL_NS);
    }

    return 0;
}
