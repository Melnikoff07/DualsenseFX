// Remote control over the local network for development. One client at a time, line based:
//   request:  one line, words separated by spaces
//   reply:    "OK ..." / "ERR ..." / "READY" (send the upload now), or "DATA <n>" + n bytes + a final "OK ..." line
// The first line must be "auth <net_token>". Files can only be touched inside the project folders below.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "net.h"
#include "haptics.h"

void LogLine(const char *fmt, ...);

NetSettings g_net_settings = { .enabled = false, .token = "", .port = 7330 };

#define RUNE_MGR_OFFSET   0x2C9A668   // main+: RuneMgr instance pointer (BotW 1.6.0, found in the code)
#define READ_MAX          0x100000
#define CHUNK             0x8000
#define IO_TIMEOUT_S      5
#define PAUSE_RESET_MS    5000    // main loop stalled this long: the console slept, sockets are dead
#define LISTEN_REFRESH_MS 30000   // sleep can kill the listener without any error, so reopen it while idle
#define CLIENT_IDLE_MS    120000

static const char *AllowedDirs[] = {
    "sdmc:/config/dualsensefx/",
    "sdmc:/config/adaptive_triggers/",
    "sdmc:/config/MissionControl/",
    "sdmc:/atmosphere/contents/42000000000d5e00/",
    "sdmc:/atmosphere/contents/010000000000bd00/",
    "sdmc:/switch/.overlays/dualsense_fx.ovl",     // the settings overlay
};

// Small buffers: this is a sysmodule, transfer memory comes from our own heap (~170 KB)
static const SocketInitConfig g_socket_config = {
    .tcp_tx_buf_size     = 0x2000,
    .tcp_rx_buf_size     = 0x2000,
    .tcp_tx_buf_max_size = 0x8000,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0x2400,
    .sb_efficiency       = 2,
    .num_bsd_sessions    = 2,
    .bsd_service_type    = BsdServiceType_User,
};

static bool g_socket_ready;
static int g_listen = -1, g_udp = -1, g_client = -1;
static bool g_authed;
static u64 g_retry_tick;
static u32 g_ip;
static u64 g_ip_check_tick;
static u64 g_last_update;
static u64 g_listen_refresh_tick;
static int g_listen_failures;
static u64 g_client_seen;
static char g_line[1024];
static size_t g_line_len;
static u8 g_buf[CHUNK];

static bool g_trig_override;
static TriggerEffects g_trig_effects;
static u64 g_trig_until;
static bool g_light_override;
static u8 g_light[3];
static u64 g_light_until;

static u64 Ms(u64 ms) {
    return armNsToTicks(ms * 1000000ULL);
}

bool NetClientConnected(void) {
    return g_client >= 0;
}

bool NetTriggerOverride(TriggerEffects *out) {
    if (g_trig_override && armGetSystemTick() >= g_trig_until) {
        g_trig_override = false;
    }
    if (g_trig_override) {
        *out = g_trig_effects;
    }
    return g_trig_override;
}

bool NetLightbarOverride(u8 *r, u8 *g, u8 *b) {
    if (g_light_override && armGetSystemTick() >= g_light_until) {
        g_light_override = false;
    }
    if (g_light_override) {
        *r = g_light[0];
        *g = g_light[1];
        *b = g_light[2];
    }
    return g_light_override;
}

// ---- socket helpers ----

static void CloseFd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void Disconnect(const char *why) {
    if (g_client >= 0) {
        LogLine("net: client disconnected (%s)", why);
    }
    CloseFd(&g_client);
    g_authed = false;
    g_line_len = 0;
}

// Waits until the client socket is ready, so a full send buffer or a slow sender doesn't drop the connection
static bool WaitReady(short events) {
    struct pollfd pfd = { .fd = g_client, .events = events };
    return poll(&pfd, 1, IO_TIMEOUT_S * 1000) > 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
}

static bool SendAll(const void *data, size_t size) {
    const u8 *p = data;
    while (size > 0) {
        ssize_t n = send(g_client, p, size, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && WaitReady(POLLOUT)) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= n;
    }
    return true;
}

static bool Reply(const char *fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(text, sizeof(text) - 1, fmt, args);
    va_end(args);
    if (len < 0) {
        return false;
    }
    if (len > (int)sizeof(text) - 2) {
        len = sizeof(text) - 2;
    }
    text[len++] = '\n';
    return SendAll(text, len);
}

static bool RecvExact(void *data, size_t size) {
    u8 *p = data;
    while (size > 0) {
        ssize_t n = recv(g_client, p, size, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && WaitReady(POLLIN)) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= n;
    }
    return true;
}

// Text payload assembled in g_buf
static size_t g_text_len;

static void Text(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf((char *)g_buf + g_text_len, sizeof(g_buf) - g_text_len, fmt, args);
    va_end(args);
    if (n > 0) {
        g_text_len += (size_t)n < sizeof(g_buf) - g_text_len ? (size_t)n : sizeof(g_buf) - g_text_len - 1;
    }
}

static bool SendText(void) {
    bool ok = Reply("DATA %u", (u32)g_text_len) && SendAll(g_buf, g_text_len) && Reply("OK");
    g_text_len = 0;
    return ok;
}

// ---- parsing ----

static int Split(char *line, char **words, int max) {
    int n = 0;
    for (char *p = strtok(line, " \t"); p && n < max; p = strtok(NULL, " \t")) {
        words[n++] = p;
    }
    return n;
}

static bool ParseHex(const char *s, u64 *out) {
    char *end;
    *out = strtoull(s, &end, 16);
    return end != s && *end == 0;
}

// base+offset with base main / heap / alias / player / pmdm / rune (the RuneMgr instance), or an absolute address
static bool ParseAddr(const NetContext *ctx, const char *s, u64 *out) {
    const char *plus = strchr(s, '+');
    u64 off = 0, base = 0;
    if (!plus) {
        return ParseHex(s, out);
    }
    if (!ParseHex(plus + 1, &off) || !ctx->attached) {
        return false;
    }
    size_t len = plus - s;
    if (len == 4 && !strncmp(s, "main", 4)) {
        base = ctx->meta->main_nso_extents.base;
    } else if (len == 4 && !strncmp(s, "heap", 4)) {
        base = ctx->meta->heap_extents.base;
    } else if (len == 5 && !strncmp(s, "alias", 5)) {
        base = ctx->meta->alias_extents.base;
    } else if (len == 6 && !strncmp(s, "player", 6)) {
        base = BotwGetPlayerActor(ctx->meta);
    } else if (len == 4 && !strncmp(s, "pmdm", 4)) {
        base = ctx->botw->pmdm;
    } else if (len == 4 && !strncmp(s, "rune", 4)) {
        if (R_FAILED(dmntchtReadCheatProcessMemory(ctx->meta->main_nso_extents.base + RUNE_MGR_OFFSET, &base, sizeof(base)))) {
            base = 0;
        }
    } else {
        return false;
    }
    if (base == 0) {
        return false;
    }
    *out = base + off;
    return true;
}

static bool PathAllowed(const char *path) {
    if (strstr(path, "..") || strchr(path, '\\')) {
        return false;
    }
    for (size_t i = 0; i < sizeof(AllowedDirs) / sizeof(AllowedDirs[0]); ++i) {
        size_t n = strlen(AllowedDirs[i]);
        if (!strncmp(path, AllowedDirs[i], n) || (strlen(path) == n - 1 && !strncmp(path, AllowedDirs[i], n - 1))) {
            return true;
        }
    }
    return false;
}

// ---- commands ----

static void CmdHelp(void) {
    Text("auth <token>                        first line of every connection\n"
         "info                                module, game and memory layout\n"
         "state                               BotW state as the module sees it\n"
         "input                               controller buttons and triggers\n"
         "read <addr> <len hex>               raw memory (max 100000), addr: main+X heap+X alias+X player+X pmdm+X rune+X or absolute\n"
         "chain <addr> <off hex>...           follow u64 pointers: [[addr]+off1]+off2 ...\n"
         "query <addr>                        memory region info\n"
         "regions <from> <to>                 memory regions in a range\n"
         "haptic <s,f,al,ar,f0,f1,dl,at,ho,de>...  play up to 4 voices (decimal fields)\n"
         "pattern <name> [amount]             play a named pattern (spaces as _)\n"
         "stop                                stop all haptic voices\n"
         "triggers <44 hex digits> [s] | off  override trigger effects (R2 block, L2 block) for s seconds (default 10)\n"
         "lightbar <r> <g> <b> [s] | off      override the lightbar for s seconds (default 10)\n"
         "ls <dir> | get <path> | put <path> <size> | rm <path>    project files only\n"
         "reload                              re-read config.ini\n"
         "reboot confirm                      reboot the console\n"
         "quit\n");
    SendText();
}

static void CmdInfo(const NetContext *ctx) {
    Text("module adaptive_triggers, net port %u\n", g_net_settings.port);
    Text("ip %s\n", inet_ntoa((struct in_addr){ .s_addr = g_ip }));
    Text("attached %u, mc %u\n", ctx->attached, ctx->mc_open);
    if (ctx->attached) {
        const DmntCheatProcessMetadata *m = ctx->meta;
        Text("pid %lu program %016lX\n", m->process_id, m->program_id);
        Text("build ");
        for (int i = 0; i < 8; ++i) {
            Text("%02X", m->main_nso_build_id[i]);
        }
        Text("\nmain  %010lX size %lX\nheap  %010lX size %lX\nalias %010lX size %lX\n",
             m->main_nso_extents.base, m->main_nso_extents.size, m->heap_extents.base, m->heap_extents.size,
             m->alias_extents.base, m->alias_extents.size);
        u64 rune = 0;
        dmntchtReadCheatProcessMemory(m->main_nso_extents.base + RUNE_MGR_OFFSET, &rune, sizeof(rune));
        Text("player %010lX\npmdm   %010lX\nrune   %010lX\n", BotwGetPlayerActor(m), ctx->botw->pmdm, rune);
    }
    SendText();
}

static void CmdState(const NetContext *ctx) {
    if (!ctx->attached) {
        Reply("ERR BotW not attached");
        return;
    }
    const BotwState *s = ctx->botw;
    Text("hearts %d (trusted %u) stamina %.1f\n", s->hearts, s->hearts_trusted, s->stamina);
    Text("bow_state %u action_flags %08X menu %u surfing %u\n", s->bow_state, s->action_flags, s->menu_open, s->surfing);
    static const char *names[] = { "sword", "bow", "arrow", "shield" };
    for (int t = 0; t < BotwSlot_Count; ++t) {
        const BotwEquippedItem *e = &s->equipped[t];
        Text("%-6s %s %d %s\n", names[t], e->present ? "yes" : "no", e->value, e->present ? e->name : "");
    }
    SendText();
}

static void CmdInput(const NetContext *ctx) {
    McControllerInput in;
    if (!ctx->mc_open || R_FAILED(mcGetControllerInput(&in))) {
        Reply("ERR no controller input (mc session opens with BotW)");
        return;
    }
    Reply("OK buttons %06X lt %u rt %u valid %u", in.buttons, in.left_trigger, in.right_trigger, in.valid);
}

static void CmdRead(const NetContext *ctx, char **w, int n) {
    u64 addr, len;
    if (n < 3 || !ParseAddr(ctx, w[1], &addr) || !ParseHex(w[2], &len) || len == 0 || len > READ_MAX) {
        Reply("ERR usage: read <addr> <len hex, max %X> (game attached)", READ_MAX);
        return;
    }
    if (!Reply("DATA %lu", len)) {
        return;
    }
    u32 failed = 0;
    for (u64 pos = 0; pos < len; pos += CHUNK) {
        u64 part = len - pos < CHUNK ? len - pos : CHUNK;
        if (R_FAILED(dmntchtReadCheatProcessMemory(addr + pos, g_buf, part))) {
            memset(g_buf, 0, part);
            ++failed;
        }
        if (!SendAll(g_buf, part)) {
            return;
        }
    }
    Reply("OK address %lX failed_chunks %u", addr, failed);
}

static void CmdChain(const NetContext *ctx, char **w, int n) {
    u64 addr;
    if (n < 2 || !ParseAddr(ctx, w[1], &addr)) {
        Reply("ERR usage: chain <addr> <off hex>...");
        return;
    }
    for (int i = 2; i < n; ++i) {
        u64 ptr = 0, off;
        if (!ParseHex(w[i], &off)) {
            Reply("ERR bad offset %s", w[i]);
            return;
        }
        if (R_FAILED(dmntchtReadCheatProcessMemory(addr, &ptr, sizeof(ptr))) || ptr == 0) {
            Reply("ERR null or unreadable pointer at %lX (step %d)", addr, i - 1);
            return;
        }
        addr = ptr + off;
    }
    Reply("OK %lX", addr);
}

static void CmdQuery(const NetContext *ctx, char **w, int n, bool range) {
    u64 from, to = 0;
    if (n < (range ? 3 : 2) || !ParseAddr(ctx, w[1], &from) || (range && !ParseAddr(ctx, w[2], &to))) {
        Reply(range ? "ERR usage: regions <from> <to>" : "ERR usage: query <addr>");
        return;
    }
    if (!range) {
        to = from + 1;
    }
    for (u64 a = from; a < to && g_text_len < sizeof(g_buf) - 128;) {
        MemoryInfo mi;
        if (R_FAILED(dmntchtQueryCheatProcessMemory(&mi, a)) || mi.size == 0) {
            Text("query failed at %lX\n", a);
            break;
        }
        Text("%010lX size %010lX perm %X type %X attr %X\n", mi.addr, mi.size, mi.perm, mi.type, mi.attr);
        a = mi.addr + mi.size;
    }
    SendText();
}

static void CmdHaptic(const NetContext *ctx, char **w, int n) {
    McHapticVoice v[MC_HAPTIC_VOICES_MAX];
    int count = 0;
    for (int i = 1; i < n && count < MC_HAPTIC_VOICES_MAX; ++i) {
        unsigned f[10];
        if (sscanf(w[i], "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9]) != 10) {
            Reply("ERR voice %d: slot,flags,amp_l,amp_r,freq_start,freq_end,delay,attack,hold,decay", i);
            return;
        }
        v[count++] = (McHapticVoice){ (u8)f[0], (u8)f[1], (u8)f[2], (u8)f[3], (u16)f[4], (u16)f[5], (u16)f[6], (u16)f[7], (u16)f[8], (u16)f[9] };
    }
    if (!ctx->attached || count == 0) {
        Reply("ERR need voices and a running BotW session");
        return;
    }
    HapticsSendVoices(v, count);
    Reply("OK %d voices", count);
}

static void CmdPattern(const NetContext *ctx, char **w, int n) {
    if (n < 2 || !ctx->mc_open) {
        Reply("ERR usage: pattern <name> [amount] (BotW running)");
        return;
    }
    for (char *p = w[1]; *p; ++p) {
        if (*p == '_') {
            *p = ' ';
        }
    }
    for (int p = 0; p < HapticPattern_Count; ++p) {
        if (!strcmp(HapticsPatternName(p), w[1])) {
            HapticsPlay(p, n > 2 ? strtof(w[2], NULL) : 1.0f);
            Reply("OK %s", w[1]);
            return;
        }
    }
    Text("unknown pattern, known:\n");
    for (int p = 0; p < HapticPattern_Count; ++p) {
        Text("  %s\n", HapticsPatternName(p));
    }
    SendText();
}

static void CmdTriggers(char **w, int n) {
    if (n >= 2 && !strcmp(w[1], "off")) {
        g_trig_override = false;
        Reply("OK");
        return;
    }
    u8 raw[sizeof(TriggerEffects)];
    if (n < 2 || strlen(w[1]) != sizeof(raw) * 2) {
        Reply("ERR usage: triggers <44 hex digits: R2 mode+10 params, L2 mode+10 params> [seconds] | off");
        return;
    }
    for (size_t i = 0; i < sizeof(raw); ++i) {
        char byte[3] = { w[1][i * 2], w[1][i * 2 + 1], 0 };
        raw[i] = strtoul(byte, NULL, 16);
    }
    memcpy(&g_trig_effects, raw, sizeof(raw));
    g_trig_until = armGetSystemTick() + Ms((n > 2 ? strtoul(w[2], NULL, 10) : 10) * 1000);
    g_trig_override = true;
    Reply("OK");
}

static void CmdLightbar(char **w, int n) {
    if (n >= 2 && !strcmp(w[1], "off")) {
        g_light_override = false;
        Reply("OK");
        return;
    }
    if (n < 4) {
        Reply("ERR usage: lightbar <r> <g> <b> [seconds] | off");
        return;
    }
    for (int i = 0; i < 3; ++i) {
        g_light[i] = strtoul(w[1 + i], NULL, 10);
    }
    g_light_until = armGetSystemTick() + Ms((n > 4 ? strtoul(w[4], NULL, 10) : 10) * 1000);
    g_light_override = true;
    Reply("OK");
}

static void CmdLs(char **w, int n) {
    if (n < 2 || !PathAllowed(w[1])) {
        Reply("ERR path not allowed");
        return;
    }
    DIR *dir = opendir(w[1]);
    if (!dir) {
        Reply("ERR can't open %s", w[1]);
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) && g_text_len < sizeof(g_buf) - 300) {
        char full[512];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", w[1], e->d_name);
        bool is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        Text("%c %10ld %s\n", is_dir ? 'd' : 'f', is_dir ? 0L : (long)st.st_size, e->d_name);
    }
    closedir(dir);
    SendText();
}

static void CmdGet(char **w, int n) {
    if (n < 2 || !PathAllowed(w[1])) {
        Reply("ERR path not allowed");
        return;
    }
    FILE *f = fopen(w[1], "rb");
    struct stat st;
    if (!f || stat(w[1], &st) != 0) {
        if (f) {
            fclose(f);
        }
        Reply("ERR can't open %s", w[1]);
        return;
    }
    if (Reply("DATA %ld", (long)st.st_size)) {
        size_t left = st.st_size;
        while (left > 0) {
            size_t part = fread(g_buf, 1, left < CHUNK ? left : CHUNK, f);
            if (part == 0) {
                memset(g_buf, 0, left < CHUNK ? left : CHUNK);
                part = left < CHUNK ? left : CHUNK;
            }
            if (!SendAll(g_buf, part)) {
                break;
            }
            left -= part;
        }
        Reply("OK");
    }
    fclose(f);
}

static void CmdPut(char **w, int n) {
    u64 size = 0;
    if (n < 3 || !PathAllowed(w[1]) || (size = strtoull(w[2], NULL, 10)) == 0 || size > 0x1000000) {
        Reply("ERR usage: put <allowed path> <size in bytes, max 16 MB>");
        return;
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.part", w[1]);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        Reply("ERR can't create %s", tmp);
        return;
    }
    Reply("READY");
    u32 crc = 0;
    bool first = true, ok = true;
    for (u64 left = size; left > 0 && ok;) {
        size_t part = left < CHUNK ? left : CHUNK;
        ok = RecvExact(g_buf, part) && fwrite(g_buf, 1, part, f) == part;
        crc = first ? crc32Calculate(g_buf, part) : crc32CalculateWithSeed(crc, g_buf, part);
        first = false;
        left -= part;
    }
    fclose(f);
    if (!ok) {
        remove(tmp);
        Disconnect("upload failed");
        return;
    }
    remove(w[1]);
    if (rename(tmp, w[1]) != 0) {
        Reply("ERR rename failed, upload kept as %s", tmp);
        return;
    }
    LogLine("net: wrote %s (%lu bytes, crc %08X)", w[1], size, crc);
    Reply("OK %lu crc %08X", size, crc);
}

// Experiment: can this background module make the Joy-Cons vibrate through the hid service while a game runs?
// jtest h|d [ms] [amp 0-100] [freq_high]: h = attached (handheld), d = detached pair (player 1)
static bool g_hid_ready;

static void CmdJoyConTest(char **w, int n) {
    if (n < 2 || (w[1][0] != 'h' && w[1][0] != 'd')) {
        Reply("ERR usage: jtest h|d [ms] [amp 0-100] [freq_high]");
        return;
    }
    u32 ms = n > 2 ? strtoul(w[2], NULL, 10) : 1000;
    float amp = (n > 3 ? strtoul(w[3], NULL, 10) : 60) / 100.0f;
    float fh = n > 4 ? strtof(w[4], NULL) : 320.0f;
    if (ms > 3000) {
        ms = 3000;
    }
    if (!g_hid_ready) {
        Result rc = hidInitialize();
        LogLine("net: jtest hidInitialize 0x%X", rc);
        if (R_FAILED(rc)) {
            Reply("ERR hidInitialize 0x%X", rc);
            return;
        }
        g_hid_ready = true;
    }
    HidVibrationDeviceHandle h[2];
    Result rc = w[1][0] == 'h' ? hidInitializeVibrationDevices(h, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)
                               : hidInitializeVibrationDevices(h, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    bool permitted = false;
    Result rp = hidIsVibrationPermitted(&permitted);
    if (R_FAILED(rc)) {
        Reply("ERR InitializeVibrationDevices 0x%X (permitted %u, rc 0x%X)", rc, permitted, rp);
        return;
    }
    HidVibrationValue v[2] = {
        { .amp_low = amp, .freq_low = 160.0f, .amp_high = amp, .freq_high = fh },
        { .amp_low = amp, .freq_low = 160.0f, .amp_high = amp, .freq_high = fh },
    };
    Result rs = 0;
    HidVibrationValue actual = {0};
    for (u32 t = 0; t < ms; t += 10) {
        rs = hidSendVibrationValues(h, v, 2);
        if (t == 200) {
            hidGetActualVibrationValue(h[0], &actual);
        }
        svcSleepThread(10000000ULL);
    }
    HidVibrationValue off[2] = {
        { .amp_low = 0, .freq_low = 160.0f, .amp_high = 0, .freq_high = 320.0f },
        { .amp_low = 0, .freq_low = 160.0f, .amp_high = 0, .freq_high = 320.0f },
    };
    hidSendVibrationValues(h, off, 2);
    LogLine("net: jtest %c send 0x%X permitted %u actual %.2f/%.0f %.2f/%.0f", w[1][0], rs, permitted,
            actual.amp_low, actual.freq_low, actual.amp_high, actual.freq_high);
    Reply("OK send 0x%X, permitted %u, actual while sending: low %.2f @ %.0f Hz, high %.2f @ %.0f Hz", rs, permitted,
          actual.amp_low, actual.freq_low, actual.amp_high, actual.freq_high);
}

static void CmdRm(char **w, int n) {
    if (n < 2 || !PathAllowed(w[1])) {
        Reply("ERR path not allowed");
        return;
    }
    if (remove(w[1]) != 0) {
        Reply("ERR can't remove %s", w[1]);
        return;
    }
    LogLine("net: removed %s", w[1]);
    Reply("OK");
}

static void HandleLine(const NetContext *ctx, char *line) {
    char *w[16];
    int n = Split(line, w, 16);
    if (n == 0) {
        return;
    }

    if (!g_authed) {
        if (n == 2 && !strcmp(w[0], "auth") && !strcmp(w[1], g_net_settings.token)) {
            g_authed = true;
            Reply("OK adaptive_triggers");
        } else {
            Reply("ERR auth required");
            Disconnect("bad auth");
        }
        return;
    }

    bool memory_cmd = !strcmp(w[0], "read") || !strcmp(w[0], "chain") || !strcmp(w[0], "query") || !strcmp(w[0], "regions");
    if (memory_cmd && !ctx->attached) {
        Reply("ERR BotW not attached (the module attaches ~30 s after the game starts)");
        return;
    }

    if      (!strcmp(w[0], "help"))     CmdHelp();
    else if (!strcmp(w[0], "info"))     CmdInfo(ctx);
    else if (!strcmp(w[0], "state"))    CmdState(ctx);
    else if (!strcmp(w[0], "input"))    CmdInput(ctx);
    else if (!strcmp(w[0], "read"))     CmdRead(ctx, w, n);
    else if (!strcmp(w[0], "chain"))    CmdChain(ctx, w, n);
    else if (!strcmp(w[0], "query"))    CmdQuery(ctx, w, n, false);
    else if (!strcmp(w[0], "regions"))  CmdQuery(ctx, w, n, true);
    else if (!strcmp(w[0], "haptic"))   CmdHaptic(ctx, w, n);
    else if (!strcmp(w[0], "pattern"))  CmdPattern(ctx, w, n);
    else if (!strcmp(w[0], "stop"))     Reply(ctx->mc_open && R_SUCCEEDED(mcStopHaptics()) ? "OK" : "ERR no mc session");
    else if (!strcmp(w[0], "triggers")) CmdTriggers(w, n);
    else if (!strcmp(w[0], "lightbar")) CmdLightbar(w, n);
    else if (!strcmp(w[0], "ls"))       CmdLs(w, n);
    else if (!strcmp(w[0], "get"))      CmdGet(w, n);
    else if (!strcmp(w[0], "put"))      CmdPut(w, n);
    else if (!strcmp(w[0], "rm"))       CmdRm(w, n);
    else if (!strcmp(w[0], "jtest"))    CmdJoyConTest(w, n);
    else if (!strcmp(w[0], "reload"))   { ReloadConfig(); LogLine("net: config reloaded"); Reply("OK"); }
    else if (!strcmp(w[0], "reboot") && n == 2 && !strcmp(w[1], "confirm")) {
        LogLine("net: reboot requested");
        Reply("OK rebooting");
        if (R_SUCCEEDED(bpcInitialize())) {
            bpcRebootSystem();
        }
        Reply("ERR reboot failed");
    }
    else if (!strcmp(w[0], "quit"))     { Reply("OK bye"); Disconnect("quit"); }
    else                                Reply("ERR unknown command, try help");
}

// ---- server ----

static void CloseListeners(void) {
    CloseFd(&g_listen);
    CloseFd(&g_udp);
}

// Drop everything and start the socket service again a moment later
static void ResetSockets(const char *why) {
    LogLine("net: restarting network (%s)", why);
    Disconnect(why);
    CloseListeners();
    if (g_socket_ready) {
        socketExit();
        g_socket_ready = false;
    }
    g_listen_failures = 0;
    g_ip = 0;
    g_retry_tick = armGetSystemTick() + Ms(2000);
}

static void OpenListeners(bool quiet) {
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    int yes = 1;

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_port = htons(g_net_settings.port);
    if (g_listen < 0 || setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(g_listen, 1) < 0) {
        LogLine("net: TCP listen on %u failed (errno %d)", g_net_settings.port, errno);
        CloseFd(&g_listen);
        g_listen_failures++;
        return;
    }
    g_listen_failures = 0;
    g_listen_refresh_tick = armGetSystemTick() + Ms(LISTEN_REFRESH_MS);
    fcntl(g_listen, F_SETFL, O_NONBLOCK);

    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_port = htons(g_net_settings.port + 1);
    if (g_udp < 0 || bind(g_udp, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LogLine("net: UDP discovery on %u failed (errno %d)", g_net_settings.port + 1, errno);
        CloseFd(&g_udp);
    } else {
        fcntl(g_udp, F_SETFL, O_NONBLOCK);
    }
    if (!quiet) {
        LogLine("net: listening on TCP %u (discovery UDP %u)", g_net_settings.port, g_net_settings.port + 1);
    }
}

static void Discovery(void) {
    char msg[32];
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    ssize_t n = recvfrom(g_udp, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&from, &len);
    if (n <= 0) {
        return;
    }
    msg[n] = 0;
    if (strncmp(msg, "atdiscover", 10) == 0) {
        char reply[64];
        int r = snprintf(reply, sizeof(reply), "adaptive_triggers %u", g_net_settings.port);
        sendto(g_udp, reply, r, 0, (struct sockaddr *)&from, len);
    }
}

static void Accept(void) {
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int fd = accept(g_listen, (struct sockaddr *)&from, &len);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LogLine("net: accept failed (errno %d), reopening", errno);
            CloseListeners();
            g_retry_tick = armGetSystemTick() + Ms(3000);
        }
        return;
    }
    // The listener is non-blocking and accepted sockets inherit that: large replies and uploads failed half way
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    g_client = fd;
    g_client_seen = armGetSystemTick();
    g_authed = false;
    g_line_len = 0;
    LogLine("net: client %s connected", inet_ntoa(from.sin_addr));
}

static void ReadClient(const NetContext *ctx) {
    struct pollfd pfd = { .fd = g_client, .events = POLLIN };
    while (g_client >= 0 && poll(&pfd, 1, 0) > 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            Disconnect("connection closed");
            return;
        }
        char c;
        ssize_t n = recv(g_client, &c, 1, 0);
        if (n <= 0) {
            Disconnect("connection closed");
            return;
        }
        g_client_seen = armGetSystemTick();
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (g_line_len < sizeof(g_line) - 1) {
                g_line[g_line_len++] = c;
            }
            continue;
        }
        g_line[g_line_len] = 0;
        g_line_len = 0;
        HandleLine(ctx, g_line);
    }
}

void NetUpdate(const NetContext *ctx) {
    if (!g_net_settings.enabled) {
        if (g_client >= 0 || g_listen >= 0) {
            Disconnect("net disabled");
            CloseListeners();
        }
        return;
    }
    u64 now = armGetSystemTick();
    if (g_last_update != 0 && now - g_last_update > Ms(PAUSE_RESET_MS) && g_socket_ready) {
        ResetSockets("woke up after a pause");
    }
    g_last_update = now;

    if (!g_socket_ready) {
        if (now < g_retry_tick) {
            return;
        }
        if (g_net_settings.token[0] == 0) {
            LogLine("net: net_token is empty, network control stays off");
            g_net_settings.enabled = false;
            return;
        }
        Result rc = socketInitialize(&g_socket_config);
        if (R_FAILED(rc)) {
            LogLine("net: socket init failed 0x%X, retrying", rc);
            g_retry_tick = now + Ms(10000);
            return;
        }
        g_socket_ready = true;
    }

    if (g_listen < 0) {
        if (now < g_retry_tick) {
            return;
        }
        OpenListeners(false);
        if (g_listen < 0) {
            if (g_listen_failures >= 2) {
                ResetSockets("cannot listen");
            } else {
                g_retry_tick = now + Ms(5000);
            }
            return;
        }
    }

    if (g_client < 0 && now >= g_listen_refresh_tick) {
        CloseListeners();
        OpenListeners(true);
        if (g_listen < 0) {
            g_retry_tick = now + Ms(2000);
            return;
        }
    }
    if (g_client >= 0 && now - g_client_seen > Ms(CLIENT_IDLE_MS)) {
        Disconnect("idle");
    }

    // The address can change (Wi-Fi reconnects): log it when it does
    if (now >= g_ip_check_tick) {
        g_ip_check_tick = now + Ms(10000);
        u32 ip = gethostid();
        if (ip != g_ip) {
            g_ip = ip;
            LogLine("net: console address %s", inet_ntoa((struct in_addr){ .s_addr = ip }));
        }
    }

    if (g_udp >= 0) {
        Discovery();
    }
    if (g_client < 0) {
        Accept();
    }
    if (g_client >= 0) {
        ReadClient(ctx);
    }
}
