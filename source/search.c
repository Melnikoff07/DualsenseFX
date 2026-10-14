// Correlation search for game state that has no known address yet (bow drawn, shield raised).
//
// The player stands still and repeatedly holds ZR (or ZL) for ~2 s, then releases for ~2 s.
// - After 1 s with both triggers released the region is snapshotted as the idle baseline.
//   Words that differ between two consecutive baselines are counted as noise.
// - After 0.8 s of a trigger held the region is compared with the baseline: every word that
//   differs gets a hit for that trigger.
// Words with a hit on every cycle and little noise are logged as candidates.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "search.h"
#include "botw.h"
#include "mc.h"

void LogLine(const char *fmt, ...);

SearchSettings g_search_settings = {
    .enabled = false,
    .offset = 0,
    .size = 0x4000,
};

#define PRESSED          200
#define RELEASED         20
#define IDLE_SAMPLES     20  // 1 s at 20 Hz
#define HOLD_SAMPLES     16  // 0.8 s
#define REPORT_EVERY     5   // cycles
#define MAX_CANDIDATES   40

typedef struct {
    const char *name;
    u32 hold;
    u32 cycles;
    u8 *hits;
} SearchTrigger;

static struct {
    bool active;
    u64 base;
    u32 words;
    u32 *cur;
    u32 *baseline;
    u32 *prev_baseline;
    u8 *noise;
    bool have_baseline;
    bool have_prev_baseline;
    bool armed[2];
    u32 idle;
    u32 baselines;
    u32 samples;
    u64 player;
    SearchTrigger trig[2];
} g;

void SearchStop(void) {
    free(g.cur);
    free(g.baseline);
    free(g.prev_baseline);
    free(g.noise);
    free(g.trig[0].hits);
    free(g.trig[1].hits);
    memset(&g, 0, sizeof(g));
}

bool SearchStart(const DmntCheatProcessMetadata *meta) {
    SearchStop();
    if (!g_search_settings.enabled) {
        return true;
    }

    u64 player = BotwGetPlayerActor(meta);
    if (player == 0) {
        return false;
    }
    g.player = player;

    g.words = g_search_settings.size / 4;
    g.base = player + g_search_settings.offset;
    g.cur           = malloc(g.words * 4);
    g.baseline      = malloc(g.words * 4);
    g.prev_baseline = malloc(g.words * 4);
    g.noise         = calloc(g.words, 1);
    g.trig[0].hits  = calloc(g.words, 1);
    g.trig[1].hits  = calloc(g.words, 1);
    g.trig[0].name  = "ZR";
    g.trig[1].name  = "ZL";
    if (!g.cur || !g.baseline || !g.prev_baseline || !g.noise || !g.trig[0].hits || !g.trig[1].hits) {
        LogLine("search: out of memory for %u bytes", g_search_settings.size);
        SearchStop();
        return true;
    }

    g.active = true;
    LogLine("search: player actor %010lX, region +%X..+%X", player, g_search_settings.offset, g_search_settings.offset + g_search_settings.size);
    return true;
}

static void Report(SearchTrigger *t) {
    LogLine("search: %s candidates after %u cycles (%u idle baselines):", t->name, t->cycles, g.baselines);

    u32 shown = 0;
    u32 max_noise = g.baselines / 4;
    for (u32 noise = 0; noise <= max_noise && shown < MAX_CANDIDATES; ++noise) {
        for (u32 i = 0; i < g.words && shown < MAX_CANDIDATES; ++i) {
            if (t->hits[i] != t->cycles || g.noise[i] != noise) {
                continue;
            }
            float fi, fh;
            memcpy(&fi, &g.baseline[i], 4);
            memcpy(&fh, &g.cur[i], 4);
            LogLine("  +%05X idle %08X held %08X  (f %g -> %g)  noise %u", g_search_settings.offset + i * 4, g.baseline[i], g.cur[i], fi, fh, noise);
            ++shown;
        }
    }
    if (shown == 0) {
        LogLine("  none");
    }
}

void SearchUpdate(const DmntCheatProcessMetadata *meta) {
    if (!g.active) {
        return;
    }

    // The player actor is recreated on death / save reload: start over on the new one
    if (++g.samples % 20 == 0) {
        u64 player = BotwGetPlayerActor(meta);
        if (player != 0 && player != g.player) {
            LogLine("search: player actor changed, restarting (stats so far: ZR %u, ZL %u cycles)", g.trig[0].cycles, g.trig[1].cycles);
            SearchStart(meta);
            return;
        }
    }

    McAnalogTriggers trig;
    if (R_FAILED(mcGetAnalogTriggers(&trig)) || !trig.valid) {
        return;
    }
    if (R_FAILED(dmntchtReadCheatProcessMemory(g.base, g.cur, g.words * 4))) {
        return;
    }

    bool held[2] = { trig.right > PRESSED, trig.left > PRESSED };
    bool released = trig.right < RELEASED && trig.left < RELEASED;

    if (released) {
        g.trig[0].hold = g.trig[1].hold = 0;
        if (++g.idle == IDLE_SAMPLES) {
            if (g.have_baseline) {
                memcpy(g.prev_baseline, g.baseline, g.words * 4);
                g.have_prev_baseline = true;
            }
            memcpy(g.baseline, g.cur, g.words * 4);
            g.have_baseline = true;
            if (g.have_prev_baseline) {
                ++g.baselines;
                for (u32 i = 0; i < g.words; ++i) {
                    if (g.baseline[i] != g.prev_baseline[i] && g.noise[i] < 255) {
                        ++g.noise[i];
                    }
                }
            }
            g.armed[0] = g.armed[1] = true;
        }
        return;
    }
    g.idle = 0;

    for (int t = 0; t < 2; ++t) {
        SearchTrigger *st = &g.trig[t];
        if (!held[t] || held[1 - t]) {
            st->hold = 0;
            continue;
        }
        if (++st->hold != HOLD_SAMPLES || !g.armed[t] || !g.have_baseline) {
            continue;
        }

        g.armed[t] = false;
        ++st->cycles;
        for (u32 i = 0; i < g.words; ++i) {
            if (g.cur[i] != g.baseline[i] && st->hits[i] < 255) {
                ++st->hits[i];
            }
        }
        LogLine("search: %s cycle %u", st->name, st->cycles);
        if (st->cycles % REPORT_EVERY == 0) {
            Report(st);
        }
    }
}
