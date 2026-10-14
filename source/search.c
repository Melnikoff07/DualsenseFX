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

// Rune mode: the player stands still, presses L once (rune in use: Magnesis / Stasis scope, bomb in hand) and keeps
// it in use for over a second, then cancels and stands idle for a few seconds. Idle baselines are taken after 1 s
// with no buttons at all; 1 s after the L press the region is compared with the baseline. Pressing B within that
// second drops the cycle. Words that differ on every cycle with little noise between baselines are candidates,
// reported per block of 5 cycles (a block per rune).

// Deep rune mode: the same, but over the first 256 bytes of every heap object that the player actor region points to
// (one level of pointers, collected when the search starts). Candidates are reported as [player+X]+Y.

// Surf mode: positives are snapshots taken when the shield wears down while surfing, negatives are
// snapshots from the start of the session, before the first surf (the player runs around without surfing).
// Surfing starts a few seconds before the first wear event, so negatives are held back in 5 s epochs and
// only committed 10-15 s later; pending epochs are dropped on the first surf event.
// A bit is a candidate when it is set in every positive and in no committed negative.

bool g_search_surf_event = false;

SearchSettings g_search_settings = {
    .enabled = false,
    .mode = SearchMode_Triggers,
    .offset = 0,
    .size = 0x4000,
};

#define PRESSED          200
#define RELEASED         20
#define IDLE_SAMPLES     20  // 1 s at 20 Hz
#define HOLD_SAMPLES     16  // 0.8 s
#define REPORT_EVERY     5   // cycles
#define MAX_CANDIDATES   40
#define SURF_EPOCHS      3
#define SURF_EPOCH_SAMPLES 100  // 5 s at 20 Hz
#define SURF_NEG_EVERY   10     // one negative snapshot per 0.5 s
#define RUNE_DELAY_SAMPLES 20   // compare 1 s after the L press
#define DEEP_MAX_OBJECTS   512  // deep rune mode: heap objects referenced from the player actor region
#define DEEP_OBJECT_BYTES  0x100
#define DEEP_OBJECT_WORDS  (DEEP_OBJECT_BYTES / 4)

static u64 g_deep_addr[DEEP_MAX_OBJECTS];
static u32 g_deep_src[DEEP_MAX_OBJECTS];    // offset in the player actor where the pointer was found
static u32 g_deep_count;

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

    // surf mode
    u32 *and_pos;
    u32 *or_neg;                     // committed negatives
    u32 *epoch_or[SURF_EPOCHS];      // pending negatives, [0] is the current epoch
    u32 epoch_count[SURF_EPOCHS];
    u32 positives;
    u32 negatives;
    bool surfed;
    u32 followup;                    // samples until an extra positive right after a wear event

    // rune mode
    bool prev_l;
    u32 rune_wait;                   // samples left until the post-L comparison
    u32 rune_blocks;
} g;

static void ReportSurf(void);

void SearchStop(void) {
    if (g.active && g_search_settings.mode == SearchMode_Surf && g.positives > 0) {
        LogLine("search: final report");
        ReportSurf();
    }
    free(g.cur);
    free(g.baseline);
    free(g.prev_baseline);
    free(g.noise);
    free(g.trig[0].hits);
    free(g.trig[1].hits);
    free(g.and_pos);
    free(g.or_neg);
    for (int e = 0; e < SURF_EPOCHS; ++e) {
        free(g.epoch_or[e]);
    }
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
    if (g_search_settings.base_rune) {
        // RuneMgr instance pointer, main+2C9A668 in 1.6.0 (from the code that spawns remote bombs)
        u64 rune = 0;
        if (R_FAILED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + 0x2C9A668, &rune, sizeof(rune))) || rune == 0) {
            return false;
        }
        g.base = rune + g_search_settings.offset;
        LogLine("search: region relative to RuneMgr %010lX", rune);
    }

    if (g_search_settings.mode == SearchMode_RuneDeep) {
        // Collect the distinct heap pointers in the player actor region
        u64 *region = malloc(g_search_settings.size);
        if (!region || R_FAILED(dmntchtReadCheatProcessMemory(g.base, region, g_search_settings.size))) {
            free(region);
            LogLine("search: player region read failed");
            return false;
        }
        u64 heap_lo = meta->heap_extents.base, heap_hi = meta->heap_extents.base + meta->heap_extents.size;
        g_deep_count = 0;
        for (u32 i = 0; i < g_search_settings.size / 8 && g_deep_count < DEEP_MAX_OBJECTS; ++i) {
            u64 p = region[i];
            if (p < heap_lo || p >= heap_hi || (p & 7) || (p >= player && p < player + 0x10000)) {
                continue;
            }
            bool seen = false;
            for (u32 k = 0; k < g_deep_count && !seen; ++k) {
                seen = g_deep_addr[k] == p;
            }
            if (!seen) {
                g_deep_addr[g_deep_count] = p;
                g_deep_src[g_deep_count] = g_search_settings.offset + i * 8;
                ++g_deep_count;
            }
        }
        free(region);

        g.words = g_deep_count * DEEP_OBJECT_WORDS;
        g.cur          = malloc(g.words * 4);
        g.baseline     = malloc(g.words * 4);
        g.noise        = calloc(g.words, 1);
        g.trig[0].hits = calloc(g.words, 1);
        g.trig[0].name = "L";
        if (g.words == 0 || !g.cur || !g.baseline || !g.noise || !g.trig[0].hits) {
            LogLine("search: deep mode, %u objects, out of memory", g_deep_count);
            SearchStop();
            return true;
        }
        g.active = true;
        LogLine("search: deep rune mode, player actor %010lX, %u heap objects referenced from +%X..+%X",
                player, g_deep_count, g_search_settings.offset, g_search_settings.offset + g_search_settings.size);
        return true;
    }

    if (g_search_settings.mode == SearchMode_Surf) {
        g.cur     = malloc(g.words * 4);
        g.and_pos = malloc(g.words * 4);
        g.or_neg  = calloc(g.words, 4);
        bool ok = g.cur && g.and_pos && g.or_neg;
        for (int e = 0; e < SURF_EPOCHS; ++e) {
            g.epoch_or[e] = calloc(g.words, 4);
            ok &= g.epoch_or[e] != NULL;
        }
        if (!ok) {
            LogLine("search: out of memory for %u bytes", g_search_settings.size);
            SearchStop();
            return true;
        }
        memset(g.and_pos, 0xFF, g.words * 4);
        g.player = player;
        g.active = true;
        g_search_surf_event = false;
        LogLine("search: surf mode, player actor %010lX, region +%X..+%X", player, g_search_settings.offset, g_search_settings.offset + g_search_settings.size);
        return true;
    }

    g.cur           = malloc(g.words * 4);
    g.baseline      = malloc(g.words * 4);
    g.prev_baseline = malloc(g.words * 4);
    g.noise         = calloc(g.words, 1);
    g.trig[0].hits  = calloc(g.words, 1);
    g.trig[1].hits  = calloc(g.words, 1);
    g.trig[0].name  = g_search_settings.mode == SearchMode_Rune ? "L" : "ZR";
    g.trig[1].name  = "ZL";
    if (!g.cur || !g.baseline || !g.prev_baseline || !g.noise || !g.trig[0].hits || !g.trig[1].hits) {
        LogLine("search: out of memory for %u bytes", g_search_settings.size);
        SearchStop();
        return true;
    }

    g.active = true;
    LogLine("search: %s mode, player actor %010lX, region +%X..+%X", g_search_settings.mode == SearchMode_Rune ? "rune" : "trigger",
            player, g_search_settings.offset, g_search_settings.offset + g_search_settings.size);
    return true;
}

static bool Snapshot(void) {
    if (g_search_settings.mode != SearchMode_RuneDeep) {
        return R_SUCCEEDED(dmntchtReadCheatProcessMemory(g.base, g.cur, g.words * 4));
    }
    for (u32 k = 0; k < g_deep_count; ++k) {
        u32 *dst = &g.cur[k * DEEP_OBJECT_WORDS];
        if (R_FAILED(dmntchtReadCheatProcessMemory(g_deep_addr[k], dst, DEEP_OBJECT_BYTES))) {
            memset(dst, 0, DEEP_OBJECT_BYTES);
        }
    }
    return true;
}

static void LogCandidate(u32 i, u32 noise) {
    float fi, fh;
    memcpy(&fi, &g.baseline[i], 4);
    memcpy(&fh, &g.cur[i], 4);
    if (g_search_settings.mode == SearchMode_RuneDeep) {
        u32 k = i / DEEP_OBJECT_WORDS;
        LogLine("  [player+%X]+%02X  (object %010lX)  idle %08X active %08X  (f %g -> %g)  noise %u",
                g_deep_src[k], (i % DEEP_OBJECT_WORDS) * 4, g_deep_addr[k], g.baseline[i], g.cur[i], fi, fh, noise);
    } else {
        LogLine("  +%05X idle %08X held %08X  (f %g -> %g)  noise %u", g_search_settings.offset + i * 4, g.baseline[i], g.cur[i], fi, fh, noise);
    }
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
            LogCandidate(i, noise);
            ++shown;
        }
    }
    if (shown == 0) {
        LogLine("  none");
    }
}

static void ReportSurf(void) {
    LogLine("search: surf candidates after %u positives, %u negatives:", g.positives, g.negatives);
    u32 shown = 0;
    for (u32 i = 0; i < g.words && shown < MAX_CANDIDATES; ++i) {
        u32 only_on = g.and_pos[i] & ~g.or_neg[i];  // set while surfing, never otherwise
        if (only_on) {
            LogLine("  +%05X now %08X  bits set only while surfing %08X", g_search_settings.offset + i * 4, g.cur[i], only_on);
            ++shown;
        }
    }
    if (shown == 0) {
        LogLine("  none");
    }
}

static void SearchUpdateSurf(void) {
    bool event = g_search_surf_event;
    g_search_surf_event = false;

    // One more positive 0.25 s after a wear event, the player is almost certainly still surfing
    if (event) {
        g.followup = 5;
    } else if (g.followup > 0 && --g.followup == 0) {
        event = true;
    }

    if (event) {
        if (!g.surfed) {
            // The last 10-15 s may already contain surfing: drop the pending epochs
            g.surfed = true;
            for (int e = 0; e < SURF_EPOCHS; ++e) {
                memset(g.epoch_or[e], 0, g.words * 4);
                g.epoch_count[e] = 0;
            }
            LogLine("search: first surf, %u negatives committed", g.negatives);
        }
    } else {
        if (g.surfed) {
            return;
        }
        ++g.samples;
        if (g.samples % SURF_EPOCH_SAMPLES == 0) {
            // Commit the oldest epoch and shift
            for (u32 i = 0; i < g.words; ++i) {
                g.or_neg[i] |= g.epoch_or[SURF_EPOCHS - 1][i];
            }
            g.negatives += g.epoch_count[SURF_EPOCHS - 1];
            u32 *oldest = g.epoch_or[SURF_EPOCHS - 1];
            for (int e = SURF_EPOCHS - 1; e > 0; --e) {
                g.epoch_or[e] = g.epoch_or[e - 1];
                g.epoch_count[e] = g.epoch_count[e - 1];
            }
            memset(oldest, 0, g.words * 4);
            g.epoch_or[0] = oldest;
            g.epoch_count[0] = 0;
        }
        if (g.samples % SURF_NEG_EVERY != 0) {
            return;
        }
    }

    if (R_FAILED(dmntchtReadCheatProcessMemory(g.base, g.cur, g.words * 4))) {
        return;
    }

    if (event) {
        for (u32 i = 0; i < g.words; ++i) {
            g.and_pos[i] &= g.cur[i];
        }
        ++g.positives;
        LogLine("search: surf positive %u (%u negatives)", g.positives, g.negatives);
        if (g.positives >= 2) {
            ReportSurf();
        }
    } else {
        for (u32 i = 0; i < g.words; ++i) {
            g.epoch_or[0][i] |= g.cur[i];
        }
        ++g.epoch_count[0];
    }
}

static void TakeBaseline(void) {
    // Words that differ between two consecutive idle baselines count as noise
    if (g.have_baseline) {
        ++g.baselines;
        for (u32 i = 0; i < g.words; ++i) {
            if (g.cur[i] != g.baseline[i] && g.noise[i] < 255) {
                ++g.noise[i];
            }
        }
    }
    memcpy(g.baseline, g.cur, g.words * 4);
    g.have_baseline = true;
}

static void SearchUpdateRune(void) {
    McControllerInput in;
    if (R_FAILED(mcGetControllerInput(&in)) || !in.valid) {
        return;
    }
    bool l_down = (in.buttons & MC_BUTTON_L) != 0;
    bool l_pressed = l_down && !g.prev_l;
    g.prev_l = l_down;
    bool any = in.buttons != 0 || in.left_trigger > RELEASED || in.right_trigger > RELEASED;

    SearchTrigger *st = &g.trig[0];
    if (g.rune_wait > 0) {
        if (in.buttons & MC_BUTTON_B) {
            g.rune_wait = 0;
            LogLine("search: L cycle dropped (B pressed within 1 s)");
            return;
        }
        if (--g.rune_wait > 0 || !Snapshot()) {
            return;
        }
        ++st->cycles;
        for (u32 i = 0; i < g.words; ++i) {
            if (g.cur[i] != g.baseline[i] && st->hits[i] < 255) {
                ++st->hits[i];
            }
        }
        LogLine("search: block %u, L cycle %u", g.rune_blocks + 1, st->cycles);
        if (st->cycles == REPORT_EVERY) {
            // Each block of 5 cycles is reported on its own, so one session can cover several runes in turn
            Report(st);
            LogLine("search: block %u done", ++g.rune_blocks);
            memset(st->hits, 0, g.words);
            st->cycles = 0;
        }
        return;
    }

    if (!any) {
        if (++g.idle == IDLE_SAMPLES && Snapshot()) {
            TakeBaseline();
            g.armed[0] = true;
        }
        return;
    }
    g.idle = 0;

    if (l_pressed && g.armed[0] && g.have_baseline) {
        g.armed[0] = false;
        g.rune_wait = RUNE_DELAY_SAMPLES;
    }
}

void SearchUpdate(const DmntCheatProcessMetadata *meta) {
    if (!g.active) {
        return;
    }

    // The player actor may be recreated on death / save reload: start over on the new one
    if (g_search_settings.mode != SearchMode_Surf && ++g.samples % 20 == 0) {
        u64 player = BotwGetPlayerActor(meta);
        if (player != 0 && player != g.player) {
            LogLine("search: player actor changed, restarting (stats so far: ZR %u, ZL %u cycles)", g.trig[0].cycles, g.trig[1].cycles);
            SearchStart(meta);
            return;
        }
    }

    if (g_search_settings.mode == SearchMode_Surf) {
        static u32 checks;
        if (++checks % 20 == 0) {
            u64 player = BotwGetPlayerActor(meta);
            if (player != 0 && player != g.player) {
                LogLine("search: player actor changed, restarting surf search (%u positives lost)", g.positives);
                SearchStart(meta);
                return;
            }
        }
        SearchUpdateSurf();
        return;
    }

    if (g_search_settings.mode == SearchMode_Rune || g_search_settings.mode == SearchMode_RuneDeep) {
        SearchUpdateRune();
        return;
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
