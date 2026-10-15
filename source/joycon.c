// Haptic patterns on Joy-Con / Pro Controller HD rumble through the hid service.
//
// Verified on console (FW 22.5.0): a background sysmodule can open the vibration devices of the attached Joy-Cons
// (handheld) and of a detached pair (player 1) and make them vibrate while BotW is in front; the system reports
// vibration permitted and the values arrive (hidGetActualVibrationValue).
// Every 5 ms the voices (same meaning as the DualSense ones in MissionControl: slot, sine or noise, amplitude per hand,
// frequency sweep, delay / attack / hold / decay with a quadratic tail, loop) are turned into one HD rumble value per
// motor: voices below 240 Hz go to the low band, the others to the high band, amplitudes add up, the frequency is the
// amplitude-weighted mean; noise is a sine whose frequency and amplitude jitter every tick. The game's own rumble is
// read back before every send and the stronger of the two plays in each band; when our voices end the game's value
// is put back. Nothing is sent while no voice plays.
#include <math.h>
#include <string.h>
#include "joycon.h"

void LogLine(const char *fmt, ...);

#define SLOTS        12
#define TICK_MS      5
#define LOW_SPLIT_HZ 240.0f

typedef struct {
    bool active, noise, loop;
    float amp[2];           // left, right
    float freq_start, freq_end;
    u32 delay, attack, hold, decay;     // ms
    u32 position;                       // ms
} Voice;

static Voice g_voices[SLOTS];
static Mutex g_lock;
static Thread g_thread;
static bool g_started;
static u32 g_seed = 0x12345678;

static HidVibrationDeviceHandle g_handles[2];
static u32 g_num_handles;
static u32 g_style;                 // style the handles were made for, 0 = none
static HidNpadIdType g_id;
static HidVibrationValue g_game[2]; // the game's latest value per motor
static HidVibrationValue g_sent[2]; // what we sent last
static bool g_playing;

static float Rand01(void) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) / 16777216.0f;
}

static float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static bool SameValue(const HidVibrationValue *a, const HidVibrationValue *b) {
    return fabsf(a->amp_low - b->amp_low) < 0.001f && fabsf(a->amp_high - b->amp_high) < 0.001f &&
           fabsf(a->freq_low - b->freq_low) < 0.5f && fabsf(a->freq_high - b->freq_high) < 0.5f;
}

// The controller in use: attached Joy-Cons, else player 1 (Joy-Con pair or Pro Controller)
static void RefreshHandles(void) {
    HidNpadIdType id = HidNpadIdType_No1;
    u32 style = 0;
    if (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) {
        id = HidNpadIdType_Handheld;
        style = HidNpadStyleTag_NpadHandheld;
    } else {
        u32 s = hidGetNpadStyleSet(HidNpadIdType_No1);
        style = (s & HidNpadStyleTag_NpadJoyDual) ? HidNpadStyleTag_NpadJoyDual :
                (s & HidNpadStyleTag_NpadFullKey) ? HidNpadStyleTag_NpadFullKey : 0;
    }
    if (style == g_style && id == g_id) {
        return;
    }
    g_style = style;
    g_id = id;
    g_num_handles = 0;
    if (style != 0 && R_SUCCEEDED(hidInitializeVibrationDevices(g_handles, 2, id, (HidNpadStyleTag)style))) {
        g_num_handles = 2;
    }
    memset(g_game, 0, sizeof(g_game));
    memset(g_sent, 0, sizeof(g_sent));
    LogLine("joycon: %s, %u motors", style == HidNpadStyleTag_NpadHandheld ? "attached Joy-Cons" :
            style == HidNpadStyleTag_NpadJoyDual ? "Joy-Con pair" : style == HidNpadStyleTag_NpadFullKey ? "Pro Controller" : "no controller",
            g_num_handles);
}

// One 5 ms step of all voices into a value per motor; false when nothing plays
static bool Mix(HidVibrationValue out[2]) {
    float amp[2][2] = {{0}}, fsum[2][2] = {{0}};    // [motor][band]
    bool any = false;
    for (u32 i = 0; i < SLOTS; ++i) {
        Voice *v = &g_voices[i];
        if (!v->active) {
            continue;
        }
        any = true;
        u32 t = v->position;
        v->position += TICK_MS;
        if (t < v->delay) {
            continue;
        }
        t -= v->delay;
        float env;
        u32 sustain_end = v->attack + v->hold;
        if (t < v->attack) {
            env = (float)t / v->attack;
        } else if (v->loop || t < sustain_end) {
            env = 1.0f;
        } else if (t < sustain_end + v->decay) {
            float x = 1.0f - (float)(t - sustain_end) / v->decay;
            env = x * x;
        } else {
            v->active = false;
            continue;
        }
        u32 sweep = v->loop ? sustain_end : sustain_end + v->decay;
        float progress = sweep > 0 ? fminf(1.0f, (float)t / sweep) : 1.0f;
        float freq = v->freq_start + (v->freq_end - v->freq_start) * progress;
        float jitter = 1.0f;
        if (v->noise) {
            freq *= 0.75f + 0.5f * Rand01();
            jitter = 0.55f + 0.45f * Rand01();
        }
        int band = freq < LOW_SPLIT_HZ ? 0 : 1;
        for (int m = 0; m < 2; ++m) {
            float a = env * v->amp[m] * jitter;
            amp[m][band] += a;
            fsum[m][band] += a * freq;
        }
    }
    if (!any) {
        return false;
    }
    for (int m = 0; m < 2; ++m) {
        out[m].amp_low   = Clamp(amp[m][0], 0.0f, 1.0f);
        out[m].freq_low  = amp[m][0] > 0 ? Clamp(fsum[m][0] / amp[m][0], 41.0f, 626.0f) : 160.0f;
        out[m].amp_high  = Clamp(amp[m][1], 0.0f, 1.0f);
        out[m].freq_high = amp[m][1] > 0 ? Clamp(fsum[m][1] / amp[m][1], 82.0f, 1252.0f) : 320.0f;
    }
    return true;
}

static void Thread_(void *arg) {
    (void)arg;
    u64 next_refresh = 0;
    while (true) {
        u64 now = armGetSystemTick();
        mutexLock(&g_lock);
        if (now >= next_refresh) {
            RefreshHandles();
            next_refresh = now + armNsToTicks(1000000000ULL);
        }
        HidVibrationValue ours[2];
        bool playing = Mix(ours);
        if (g_num_handles == 2 && (playing || g_playing)) {
            HidVibrationValue send[2];
            for (int m = 0; m < 2; ++m) {
                // A value that is not ours came from the game
                HidVibrationValue actual;
                if (R_SUCCEEDED(hidGetActualVibrationValue(g_handles[m], &actual)) && !SameValue(&actual, &g_sent[m])) {
                    g_game[m] = actual;
                }
                if (playing) {
                    const HidVibrationValue *g = &g_game[m];
                    send[m].amp_low   = fmaxf(ours[m].amp_low, g->amp_low);
                    send[m].freq_low  = ours[m].amp_low >= g->amp_low ? ours[m].freq_low : g->freq_low;
                    send[m].amp_high  = fmaxf(ours[m].amp_high, g->amp_high);
                    send[m].freq_high = ours[m].amp_high >= g->amp_high ? ours[m].freq_high : g->freq_high;
                } else {
                    send[m] = g_game[m];    // our voices ended: give the motor back to the game
                }
            }
            if (R_SUCCEEDED(hidSendVibrationValues(g_handles, send, 2))) {
                memcpy(g_sent, send, sizeof(g_sent));
            }
        }
        g_playing = playing;
        mutexUnlock(&g_lock);
        svcSleepThread(TICK_MS * 1000000ULL);
    }
}

bool JoyConStart(void) {
    if (g_started) {
        return true;
    }
    Result rc = hidInitialize();
    if (R_FAILED(rc)) {
        LogLine("joycon: hidInitialize failed 0x%X", rc);
        return false;
    }
    mutexInit(&g_lock);
    rc = threadCreate(&g_thread, Thread_, NULL, NULL, 0x4000, 0x2C, -2);
    if (R_FAILED(rc) || R_FAILED(rc = threadStart(&g_thread))) {
        LogLine("joycon: thread failed 0x%X", rc);
        return false;
    }
    g_started = true;
    LogLine("joycon: output started");
    return true;
}

void JoyConPlay(const McHapticVoice *voices, u32 count) {
    if (!g_started) {
        return;
    }
    mutexLock(&g_lock);
    for (u32 i = 0; i < count && i < MC_HAPTIC_VOICES_MAX; ++i) {
        const McHapticVoice *in = &voices[i];
        Voice *v = &g_voices[in->slot % SLOTS];
        memset(v, 0, sizeof(*v));
        if (in->amp_left == 0 && in->amp_right == 0) {
            continue;
        }
        v->active = true;
        v->noise = in->flags & McHapticFlag_Noise;
        v->loop = in->flags & McHapticFlag_Loop;
        v->amp[0] = in->amp_left / 255.0f;
        v->amp[1] = in->amp_right / 255.0f;
        v->freq_start = Clamp(in->freq_start, 41.0f, 1252.0f);
        v->freq_end = Clamp(in->freq_end, 41.0f, 1252.0f);
        v->delay = in->delay_ms;
        v->attack = in->attack_ms;
        v->hold = in->hold_ms;
        v->decay = in->decay_ms;
    }
    mutexUnlock(&g_lock);
}

void JoyConStopAll(void) {
    if (!g_started) {
        return;
    }
    mutexLock(&g_lock);
    memset(g_voices, 0, sizeof(g_voices));
    mutexUnlock(&g_lock);
}
