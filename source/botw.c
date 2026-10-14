// BotW 1.6.0 (build id 8E9978D50BDD20B4) game state readers.
//
// Inventory: uking::ui::PauseMenuDataMgr (decomp: src/Game/UI/uiPauseMenuDataMgr.h, v1.5.0).
//  - main+2CA6D48 holds the instance pointer (from switch-cheats-db 1.6.0 codes; matches mEquippedWeapons at +447D8).
//  - mEquippedWeapons[4] is only filled while the pause menu is open, so instead we scan the pouch item
//    buffer (SafeArray<PouchItem, 420>, item size 0x298) for items with mEquipped set.
//  - Buffer start mgr+0x90 was derived from item pointers seen in the log (all at 0x90 mod 0x298).
//  - Field offsets are the decomp 1.5.0 PouchItem offsets + 8, which is what the cheat codes use
//    (value at +0x20, name chars at +0x3C). Verified on console: 1.6.0 PouchItem has a vtable at +0 that
//    the 1.5.0 decomp layout doesn't have. Values: durability x100 (-100 per hit), arrow count.
//  - mEquippedWeapons[] non-null means the pause menu is open.
//
// Player: main+2CA1A78 is the PlayerInfo instance, +80 is PlayerInfo::mPlayerActor (decomp actPlayerInfo.h),
// hearts at player actor +848 (quarter hearts). From cheat codes, verified live.
// Bow state at player actor +1F88, found with the correlation search and verified on console:
//   0 no bow action, 1 nocking an arrow (~0.2 s), 2 drawn (player +18F8 points to the nocked arrow),
//   3 arrow shot (~0.3 s, bow durability and arrow count drop here). Cancel with B goes 2 -> 0.
// Action flags at player actor +C40 (verified on console):
//   0x18 while ZL is held (guard / lock-on stance, also with two-handed weapons), 0x40000000 bow aiming,
//   0xC000 for ~0.13 s when Daruk's Protection blocks a hit.
//   0x2 for 0.2-4.5 s stretches: NOT shield surfing (stays off while surfing), probably jumping / airborne.
// Stamina: [[main+2C9FD70]+38]+2AC, f32, 1800 = one full wheel (from cheat codes, verified live).
// Shield surfing has no known flag yet, but it wears the shield down every few seconds while
// there is no ZL stance, whereas a blocked hit always happens in the ZL stance.
// Getting on the shield drops the ZL stance straight to nothing (0x18/0x98 -> 0), while lowering the
// shield normally goes through 0x88/0x80 (seen on all 5 surf starts in one session). Surfing ends on a
// jump / fall (0x2), shield raised, bow drawn, or after surf_timeout_ms without shield wear.
//
// Runes: main+2C9A668 holds the RuneMgr instance (found in the code: the bomb spawn function at +B96480 is called
// with it from the player logic). Fields recorded live over the network while using each rune:
//   +90  flags: 0x10 a rune is raised to aim (Magnesis / Stasis), 0x4 round bomb out, 0x8 cube bomb out
//   +240 selected rune: 0 round bomb, 1 cube bomb, 2 Magnesis, 3 Stasis, 4 Cryonis
//   +410 aimed rune locked on a target (pointer, 0 when not)
//   +2B0 + i*0x28 bomb slots (round, cube): +20 state 0 none, 3 out, 0xE exploding
//   +488 Stasis: 0x10000 while a target is frozen, 0x100 flashes with the frozen target (faster towards the end)
// Magnesis holding an object: the player's current tool name at player +D44 reads "Item_Magnetglove" (the rune
// flags drop the aim bit while holding).
#include <stdio.h>
#include <string.h>
#include "botw.h"
#include "haptics.h"
#include "search.h"

void LogLine(const char *fmt, ...);
void LogVerbose(const char *fmt, ...);

#define PMDM_INSTANCE_OFFSET   0x2CA6D48
#define POUCH_BUFFER_OFFSET    0x90
#define POUCH_ITEM_SIZE        0x298
#define POUCH_ITEMS_MAX        420

#define ITEM_TYPE_OFFSET       0x18
#define ITEM_VALUE_OFFSET      0x20
#define ITEM_EQUIPPED_OFFSET   0x24
#define ITEM_INVENTORY_OFFSET  0x25
#define ITEM_NAME_OFFSET       0x3C
#define ITEM_HEADER_SIZE       0x80

#define PMDM_EQUIPPED_OFFSET   0x447D8 // SafeArray<PouchItem*, 4>

#define PLAYER_INFO_OFFSET     0x2CA1A78
#define PLAYER_HEARTS_CHAIN1   0x80
#define PLAYER_HEARTS_CHAIN2   0x848
#define PLAYER_BOW_STATE       0x1F88
#define PLAYER_ACTION_FLAGS    0xC40
#define ACTION_DARUK_BLOCK     0xC000
#define ACTION_ZL_STANCE       0x18
#define ACTION_AIRBORNE        0x2   // probably
#define STAMINA_OFFSET         0x2C9FD70
#define STAMINA_CHAIN1         0x38
#define STAMINA_CHAIN2         0x2AC
#define RUNE_MGR_OFFSET        0x2C9A668
#define RUNE_FIELDS_START      0x80     // one read covers the fields below
#define RUNE_FIELDS_SIZE       0x420
#define RUNE_FLAGS             0x90
#define RUNE_FLAG_AIM          0x10
#define RUNE_TARGET            0x410
#define RUNE_BOMB_STATE        0x2D0    // + slot * 0x28
#define RUNE_BOMB_STRIDE       0x28
#define RUNE_BOMB_EXPLODING    0xE
#define RUNE_STASIS            0x488
#define STASIS_FROZEN          0x10000
#define STASIS_FLASH           0x100
#define PLAYER_TOOL_NAME       0xD44
#define RUNE_LOCK_DEDUP_NS     (500 * 1000000ULL)
#define LANDING_MIN_NS         (900 * 1000000ULL)   // shorter airborne stretches are ordinary jumps
#define BREAK_DEDUP_NS         (1000 * 1000000ULL)

#define SCAN_INTERVAL_NS       (500 * 1000000ULL)
#define SETTLE_NS              (2000 * 1000000ULL)  // after death / reload
#define PULSE_NS               (250 * 1000000ULL)
#define LOW_DURABILITY         300                  // 3 hits left
#define SCAN_CHUNK_ITEMS       12

BotwSettings g_botw_settings = {
    .shield_hit_strength = 2,
    .shield_hit_style = 2,
    .shield_hit_ms = 150,
    .low_health_quarters = 4,
    .bow_release_strength = 0,
    .bow_drawn_strength = 8,
    .daruk_strength = 5,
    .surf_strength = 3,
    .surf_rumble_strength = 2,
    .surf_timeout_ms = 6000,
};

static const u8 SupportedBuildId[8] = { 0x8E, 0x99, 0x78, 0xD5, 0x0B, 0xDD, 0x20, 0xB4 };

static const char *SlotNames[BotwSlot_Count] = { "sword", "bow", "arrow", "shield" };

static u8 g_scan_buffer[SCAN_CHUNK_ITEMS * POUCH_ITEM_SIZE];

static void CopyName(char *out, const u8 *src) {
    for (int i = 0; i < 63; ++i) {
        char c = (char)src[i];
        if (c == '\0') {
            out[i] = '\0';
            return;
        }
        out[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    out[63] = '\0';
}

static void LogHex(const char *label, const u8 *data, size_t size) {
    for (size_t off = 0; off < size; off += 16) {
        char line[16 * 3 + 1] = {0};
        for (size_t i = 0; i < 16 && off + i < size; ++i) {
            snprintf(&line[i * 3], 4, "%02X ", data[off + i]);
        }
        LogVerbose("  %s +%02lX: %s", label, off, line);
    }
}

void BotwInit(BotwState *state, const DmntCheatProcessMetadata *meta) {
    memset(state, 0, sizeof(*state));

    state->supported = memcmp(meta->main_nso_build_id, SupportedBuildId, sizeof(SupportedBuildId)) == 0;
    if (!state->supported) {
        LogLine("botw: unknown build, game state readers disabled");
        return;
    }
    LogLine("botw: 1.6.0 build recognised");
}

static void Settle(BotwState *state, const char *why) {
    u64 until = armGetSystemTick() + armNsToTicks(SETTLE_NS);
    if (until > state->settle_until_tick) {
        if (armGetSystemTick() >= state->settle_until_tick) {
            LogVerbose("botw: effects paused (%s)", why);
        }
        state->settle_until_tick = until;
    }
}

static void Pulse(BotwState *state, int trigger, TriggerEffect effect, u64 duration_ns) {
    state->pulse_effect[trigger] = effect;
    state->pulse_until_tick[trigger] = armGetSystemTick() + armNsToTicks(duration_ns);
}

static void SetSurfing(BotwState *state, bool surfing, const char *why);

// A save is loaded and gameplay is running: something is equipped and no death / reload is being settled
static bool InGame(const BotwState *state) {
    bool loaded = false;
    for (int t = 0; t < BotwSlot_Count; ++t) {
        loaded |= state->equipped[t].present;
    }
    return loaded && state->hearts_trusted && state->hearts > 0 && armGetSystemTick() >= state->settle_until_tick;
}

static void PlayBreak(BotwState *state, const char *what) {
    u64 now = armGetSystemTick();
    if (now - state->break_tick < armNsToTicks(BREAK_DEDUP_NS)) {
        return;
    }
    state->break_tick = now;
    LogVerbose("botw: %s broke", what);
    HapticsPlay(HapticPattern_WeaponBreak, 1.0f);
}

static HapticPattern HitPattern(const BotwEquippedItem *weapon) {
    if (strncmp(weapon->name, "Weapon_Lsword_", 14) == 0) {
        return HapticPattern_HitHeavy;
    }
    if (strncmp(weapon->name, "Weapon_Spear_", 13) == 0) {
        return HapticPattern_HitSpear;
    }
    return HapticPattern_HitSword;
}

static void ScanPouch(BotwState *state) {
    BotwEquippedItem found[BotwSlot_Count] = {0};
    u32 valid_items = 0;

    for (u32 first = 0; first < POUCH_ITEMS_MAX; first += SCAN_CHUNK_ITEMS) {
        u32 count = POUCH_ITEMS_MAX - first < SCAN_CHUNK_ITEMS ? POUCH_ITEMS_MAX - first : SCAN_CHUNK_ITEMS;
        u64 addr = state->pmdm + POUCH_BUFFER_OFFSET + (u64)first * POUCH_ITEM_SIZE;
        if (R_FAILED(dmntchtReadCheatProcessMemory(addr, g_scan_buffer, count * POUCH_ITEM_SIZE))) {
            LogLine("botw: pouch read failed at slot %u", first);
            return;
        }

        for (u32 i = 0; i < count; ++i) {
            const u8 *item = &g_scan_buffer[i * POUCH_ITEM_SIZE];
            s32 type;
            memcpy(&type, item + ITEM_TYPE_OFFSET, sizeof(type));
            if (type < 0 || type > 9) {
                continue;
            }
            ++valid_items;

            if (type >= BotwSlot_Count || item[ITEM_EQUIPPED_OFFSET] != 1 || item[ITEM_INVENTORY_OFFSET] != 1) {
                continue;
            }

            BotwEquippedItem *e = &found[type];
            if (e->present) {
                LogVerbose("botw: second equipped %s at slot %u", SlotNames[type], first + i);
                continue;
            }
            e->present = true;
            e->slot = first + i;
            memcpy(&e->value, item + ITEM_VALUE_OFFSET, sizeof(e->value));
            CopyName(e->name, item + ITEM_NAME_OFFSET);

            if (!state->dumped[type]) {
                state->dumped[type] = true;
                LogVerbose("botw: raw %s item, slot %u:", SlotNames[type], first + i);
                LogHex(SlotNames[type], item, ITEM_HEADER_SIZE);
            }
        }
    }

    int moved = 0;
    for (int t = 0; t < BotwSlot_Count; ++t) {
        if (state->equipped[t].present && found[t].present && state->equipped[t].slot != found[t].slot && strcmp(state->equipped[t].name, found[t].name) == 0) {
            ++moved;
        }
    }
    if (moved >= 2) {
        Settle(state, "items moved, save reloaded");
    }

    for (int t = 0; t < BotwSlot_Count; ++t) {
        BotwEquippedItem *old = &state->equipped[t];
        BotwEquippedItem *now = &found[t];
        // An equipped weapon or shield on its last hit vanished outside the menu: it broke
        if (t != BotwSlot_Arrow && old->present && !now->present && old->value > 0 && old->value <= 100 &&
            !state->menu_open && moved < 2 && InGame(state)) {
            PlayBreak(state, SlotNames[t]);
        }
        if (old->present != now->present || old->slot != now->slot || strcmp(old->name, now->name) != 0) {
            if (now->present) {
                LogVerbose("botw: equipped %-6s slot %3u %-24s value %d (%u items in pouch)", SlotNames[t], now->slot, now->name, now->value, valid_items);
            } else {
                LogVerbose("botw: equipped %-6s none", SlotNames[t]);
            }
        }
        *old = *now;
    }
}

// Re-read the value of each equipped item every poll, to see whether it updates live (without the menu)
static void TrackValues(BotwState *state) {
    for (int t = 0; t < BotwSlot_Count; ++t) {
        BotwEquippedItem *e = &state->equipped[t];
        if (!e->present) {
            continue;
        }

        u64 addr = state->pmdm + POUCH_BUFFER_OFFSET + (u64)e->slot * POUCH_ITEM_SIZE + ITEM_VALUE_OFFSET;
        s32 value;
        if (R_FAILED(dmntchtReadCheatProcessMemory(addr, &value, sizeof(value))) || value == e->value) {
            continue;
        }

        LogVerbose("botw: %-6s value %d -> %d", SlotNames[t], e->value, value);

        // While a save loads every item briefly reads 1
        if (value == 1 && e->value != 2) {
            Settle(state, "values reset, save loading");
        } else if (value < e->value && e->value - value <= 1000) {
            // Durability went down: the item was used
            bool in_game = InGame(state);
            if (in_game && t != BotwSlot_Arrow && value <= 0) {
                PlayBreak(state, SlotNames[t]);
            } else if (in_game && t == BotwSlot_Sword) {
                HapticsPlay(HitPattern(e), 1.0f);
            } else if (in_game && t == BotwSlot_Shield) {
                HapticsPlay((state->action_flags & ACTION_ZL_STANCE) ? HapticPattern_ShieldBlock : HapticPattern_SurfBump, 1.0f);
            }

            if (t == BotwSlot_Shield && !(state->action_flags & ACTION_ZL_STANCE)) {
                // Shield worn down without guarding: shield surfing. Vibration, the finger is usually just resting on the trigger
                LogVerbose("botw: shield wear while surfing");
                SetSurfing(state, true, "shield wear");
                g_search_surf_event = true;
                if (g_botw_settings.surf_strength > 0) {
                    Pulse(state, 0, TriggerEffectVibration(0, g_botw_settings.surf_strength, 30), 250 * 1000000ULL);
                    Pulse(state, 1, TriggerEffectVibration(0, g_botw_settings.surf_strength, 30), 250 * 1000000ULL);
                }
            } else if (t == BotwSlot_Shield && g_botw_settings.shield_hit_strength > 0) {
                // Block impact: the trigger pushes back for a moment, or buzzes
                u8 strength = g_botw_settings.shield_hit_strength;
                TriggerEffect impact;
                if (g_botw_settings.shield_hit_style == 2) {
                    impact = TriggerEffectVibration(0, strength, 60);
                } else {
                    const u8 stiff[10] = { strength, strength, strength, strength, strength, strength, strength, strength, strength, strength };
                    impact = TriggerEffectFeedback(stiff);
                }
                Pulse(state, 1, impact, g_botw_settings.shield_hit_ms * 1000000ULL);
            } else if (t == BotwSlot_Sword && value <= LOW_DURABILITY) {
                // Weapon about to break
                Pulse(state, 0, TriggerEffectVibration(0, 8, 30), PULSE_NS);
            }
        }
        e->value = value;
    }
}

static void UpdateBow(BotwState *state) {
    u32 bow_state = 0;
    if (state->player == 0 || R_FAILED(dmntchtReadCheatProcessMemory(state->player + PLAYER_BOW_STATE, &bow_state, sizeof(bow_state)))) {
        return;
    }
    if (bow_state == state->bow_state) {
        return;
    }

    if (InGame(state)) {
        if (bow_state == 1) {
            HapticsPlay(HapticPattern_BowNock, 1.0f);
        } else if (bow_state == 3 && state->bow_state == 2) {
            HapticsSetLoop(HapticLoop_BowTension, false);
            HapticsPlay(HapticPattern_BowRelease, 1.0f);
        }
    }

    // String released: short snap on R2
    if (bow_state == 3 && state->bow_state == 2 && g_botw_settings.bow_release_strength > 0) {
        Pulse(state, 0, TriggerEffectVibration(0, g_botw_settings.bow_release_strength, 45), 120 * 1000000ULL);
    }

    // Shot or cancelled: keep the light tension for a moment so the next pull already has it
    if (state->bow_state != 0 && (bow_state == 3 || bow_state == 0)) {
        state->bow_active_until_tick = armGetSystemTick() + armNsToTicks(1500 * 1000000ULL);
    }
    state->bow_state = bow_state;
}

static void SetSurfing(BotwState *state, bool surfing, const char *why) {
    if (surfing) {
        state->surf_until_tick = armGetSystemTick() + armNsToTicks(g_botw_settings.surf_timeout_ms * 1000000ULL);
    }
    if (surfing != state->surfing) {
        LogVerbose("botw: surfing %s (%s)", surfing ? "start" : "end", why);
        state->surfing = surfing;
    }
}

static void UpdateActionFlags(BotwState *state) {
    u32 flags = 0;
    if (state->player == 0 || R_FAILED(dmntchtReadCheatProcessMemory(state->player + PLAYER_ACTION_FLAGS, &flags, sizeof(flags)))) {
        return;
    }

    // Daruk's Protection blocked a hit: strong push on L2 and an orange flash
    if ((flags & ACTION_DARUK_BLOCK) && !(state->action_flags & ACTION_DARUK_BLOCK)) {
        LogVerbose("botw: Daruk's Protection blocked a hit");
        // Vibration on both triggers: felt whichever trigger the finger is on (often ZR while aiming),
        // a push-back would only be felt on a trigger that is being pressed
        u8 k = g_botw_settings.daruk_strength;
        if (k > 0) {
            Pulse(state, 0, TriggerEffectVibration(0, k, 40), 300 * 1000000ULL);
            Pulse(state, 1, TriggerEffectVibration(0, k, 40), 300 * 1000000ULL);
        }
        state->flash_until_tick = armGetSystemTick() + armNsToTicks(400 * 1000000ULL);
        if (InGame(state)) {
            HapticsPlay(HapticPattern_Daruk, 1.0f);
        }
    }
    if ((flags & ACTION_AIRBORNE) != (state->action_flags & ACTION_AIRBORNE)) {
        LogVerbose("botw: airborne(?) flag %s", (flags & ACTION_AIRBORNE) ? "on" : "off");
        u64 now = armGetSystemTick();
        if (flags & ACTION_AIRBORNE) {
            state->airborne_since_tick = now;
        } else if (state->airborne_since_tick != 0) {
            // Landed after a real fall (also after gliding: the paraglider shows up as airborne too)
            u64 airborne_ns = armTicksToNs(now - state->airborne_since_tick);
            if (airborne_ns >= LANDING_MIN_NS && !state->surfing && InGame(state)) {
                HapticsPlay(HapticPattern_Landing, airborne_ns / 2500000000.0f);
            }
            state->airborne_since_tick = 0;
        }
    }

    // Shield stance dropped straight to nothing: the player got on the shield
    const u32 stance_bits = ACTION_ZL_STANCE | 0x80;
    if ((state->action_flags & ACTION_ZL_STANCE) && !(flags & stance_bits) && state->equipped[BotwSlot_Shield].present) {
        SetSurfing(state, true, "stance dropped without lowering the shield");
    } else if (state->surfing) {
        if (flags & ACTION_AIRBORNE) {
            SetSurfing(state, false, "jump / fall");
        } else if (flags & stance_bits) {
            SetSurfing(state, false, "shield raised");
        }
    }
    state->action_flags = flags;
}

static void UpdatePlayer(BotwState *state, const DmntCheatProcessMetadata *meta) {
    bool in_game_before = InGame(state);    // before the new hearts value (death settles the effects)
    u64 ptr = 0;
    u32 hearts = 0;
    state->hearts = -1;
    if (R_SUCCEEDED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + PLAYER_INFO_OFFSET, &ptr, sizeof(ptr))) && ptr != 0 &&
        R_SUCCEEDED(dmntchtReadCheatProcessMemory(ptr + PLAYER_HEARTS_CHAIN1, &ptr, sizeof(ptr))) && ptr != 0 &&
        R_SUCCEEDED(dmntchtReadCheatProcessMemory(ptr + PLAYER_HEARTS_CHAIN2, &hearts, sizeof(hearts)))) {
        state->hearts = (s32)hearts;
    }
    if (state->hearts == 0) {
        Settle(state, "player died");
    }
    // On the title screen hearts read 1 while the inventory is already filled. Trust the value once it is above that,
    // or once it changes from what was read at attach (the save finished loading); that change is not an event.
    if (!state->hearts_trusted && state->hearts > 0) {
        if (state->hearts_at_attach == 0) {
            state->hearts_at_attach = state->hearts;
        }
        if (state->hearts > 1 || state->hearts != state->hearts_at_attach) {
            LogVerbose("botw: hearts %d trusted", state->hearts);
            state->hearts_trusted = true;
            state->last_hearts = state->hearts;
            in_game_before = false;
        }
    }

    if (state->hearts != state->last_hearts) {
        // Damage and healing, only once a save is loaded (hearts read 1 on the title screen and while loading)
        s32 before = state->last_hearts;
        bool was_in_game = before > 0 && in_game_before;
        if (was_in_game && state->hearts == 0) {
            HapticsPlay(HapticPattern_Damage, 1.0f);
        } else if (was_in_game && state->hearts > 0 && state->hearts < before) {
            HapticsPlay(HapticPattern_Damage, 0.35f + 0.1f * (before - state->hearts));
        } else if (was_in_game && state->hearts > before) {
            HapticsPlay(HapticPattern_Heal, 1.0f);
        }
        state->last_hearts = state->hearts;
        state->hearts_changed_tick = armGetSystemTick();
    }

    // Stamina ran out
    float stamina = -1.0f;
    if (R_SUCCEEDED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + STAMINA_OFFSET, &ptr, sizeof(ptr))) && ptr != 0 &&
        R_SUCCEEDED(dmntchtReadCheatProcessMemory(ptr + STAMINA_CHAIN1, &ptr, sizeof(ptr))) && ptr != 0 &&
        R_SUCCEEDED(dmntchtReadCheatProcessMemory(ptr + STAMINA_CHAIN2, &stamina, sizeof(stamina)))) {
        if (state->stamina > 1.0f && stamina <= 1.0f && stamina >= 0.0f && InGame(state)) {
            LogVerbose("botw: stamina ran out");
            HapticsPlay(HapticPattern_StaminaEmpty, 1.0f);
        }
        state->stamina = stamina;
    }

    u64 equipped[4] = {0};
    bool menu_open = false;
    if (R_SUCCEEDED(dmntchtReadCheatProcessMemory(state->pmdm + PMDM_EQUIPPED_OFFSET, equipped, sizeof(equipped)))) {
        menu_open = equipped[0] || equipped[1] || equipped[2] || equipped[3];
    }
    if (menu_open != state->menu_open) {
        LogVerbose("botw: pause menu %s", menu_open ? "open" : "closed");
        state->menu_open = menu_open;
    }
}

static bool IsLow(const BotwEquippedItem *e) {
    return e->present && e->value > 0 && e->value <= LOW_DURABILITY;
}

static u32 Field32(const u8 *fields, u32 offset) {
    u32 v;
    memcpy(&v, fields + offset - RUNE_FIELDS_START, sizeof(v));
    return v;
}

static void UpdateRunes(BotwState *state, const DmntCheatProcessMetadata *meta) {
    static u8 fields[RUNE_FIELDS_SIZE];
    u64 mgr = 0;
    if (state->player == 0 ||
        R_FAILED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + RUNE_MGR_OFFSET, &mgr, sizeof(mgr))) || mgr == 0 ||
        R_FAILED(dmntchtReadCheatProcessMemory(mgr + RUNE_FIELDS_START, fields, sizeof(fields)))) {
        state->runes_read = false;
        return;
    }
    char tool[17] = {0};
    bool held = R_SUCCEEDED(dmntchtReadCheatProcessMemory(state->player + PLAYER_TOOL_NAME, tool, 16)) &&
                strcmp(tool, "Item_Magnetglove") == 0;

    u32 flags = Field32(fields, RUNE_FLAGS);
    u64 target;
    memcpy(&target, fields + RUNE_TARGET - RUNE_FIELDS_START, sizeof(target));
    u32 bombs[2] = { Field32(fields, RUNE_BOMB_STATE), Field32(fields, RUNE_BOMB_STATE + RUNE_BOMB_STRIDE) };
    u32 stasis = Field32(fields, RUNE_STASIS);

    if (state->runes_read && InGame(state) && !state->menu_open) {
        u64 now = armGetSystemTick();
        if ((flags & RUNE_FLAG_AIM) && !(state->rune_flags & RUNE_FLAG_AIM)) {
            HapticsPlay(HapticPattern_RuneOn, 1.0f);
        }
        if ((flags & RUNE_FLAG_AIM) && target != 0 && state->rune_target == 0 && now - state->rune_lock_tick > armNsToTicks(RUNE_LOCK_DEDUP_NS)) {
            state->rune_lock_tick = now;
            HapticsPlay(HapticPattern_RuneLock, 1.0f);
        }
        if (held != state->magnesis_held) {
            LogVerbose("botw: magnesis %s", held ? "grab" : "drop");
            HapticsPlay(held ? HapticPattern_MagnesisGrab : HapticPattern_MagnesisDrop, 1.0f);
        }
        for (int i = 0; i < 2; ++i) {
            if (bombs[i] == state->bomb_state[i]) {
                continue;
            }
            if (bombs[i] == RUNE_BOMB_EXPLODING) {
                LogVerbose("botw: bomb %d exploded", i);
                HapticsPlay(HapticPattern_BombExplode, 1.0f);
            } else if (state->bomb_state[i] == 0) {
                HapticsPlay(HapticPattern_BombSpawn, 1.0f);
            }
        }
        bool frozen = stasis & STASIS_FROZEN, was_frozen = state->stasis_flags & STASIS_FROZEN;
        if (frozen && !was_frozen) {
            LogVerbose("botw: stasis freeze");
            HapticsPlay(HapticPattern_StasisFreeze, 1.0f);
        } else if (!frozen && was_frozen) {
            LogVerbose("botw: stasis release");
            HapticsPlay(HapticPattern_StasisRelease, 1.0f);
        } else if (frozen && (stasis & STASIS_FLASH) && !(state->stasis_flags & STASIS_FLASH)) {
            HapticsPlay(HapticPattern_StasisTick, 1.0f);
        }
    }

    state->runes_read = true;
    state->rune_flags = flags;
    state->rune_target = target;
    state->bomb_state[0] = bombs[0];
    state->bomb_state[1] = bombs[1];
    state->stasis_flags = stasis;
    state->magnesis_held = held;
}

bool BotwComputeEffects(BotwState *state, TriggerEffects *out) {
    out->right = TriggerEffectOff();
    out->left  = TriggerEffectOff();
    if (!state->supported || state->pmdm == 0) {
        return false;
    }

    u64 now = armGetSystemTick();
    bool off = state->menu_open || state->hearts <= 0 || now < state->settle_until_tick;
    if (off != state->effects_off_logged) {
        LogVerbose("botw: effects %s", off ? "off" : "on");
        state->effects_off_logged = off;
    }
    if (off) {
        HapticsStopAll();
        return true;
    }

    const BotwEquippedItem *sword  = &state->equipped[BotwSlot_Sword];
    const BotwEquippedItem *bow    = &state->equipped[BotwSlot_Bow];
    const BotwEquippedItem *arrow  = &state->equipped[BotwSlot_Arrow];
    const BotwEquippedItem *shield = &state->equipped[BotwSlot_Shield];

    // R2 follows the bow state: free until an arrow is being nocked (state 1, light tension),
    // tight once the bow is drawn (state 2), weak buzz while drawing a bow that is about to break
    static const u8 BowNocking[10] = {0, 0, 1, 2, 3, 3, 4, 4, 5, 5};
    bool bow_in_use = state->bow_state != 0 || now < state->bow_active_until_tick;
    if (bow->present && bow->value > 0 && arrow->present && arrow->value > 0 && bow_in_use) {
        if (IsLow(bow)) {
            out->right = TriggerEffectVibration(2, 3, 20);
        } else if (state->bow_state == 2) {
            u8 s8 = g_botw_settings.bow_drawn_strength;
            u8 drawn[10] = {0, (u8)(s8 / 4), (u8)(s8 / 2), (u8)(s8 * 3 / 4), s8, s8, s8, s8, s8, s8};
            out->right = TriggerEffectFeedback(drawn);
        } else {
            out->right = TriggerEffectFeedback(BowNocking);
        }
    }

    // L2 raises the shield, only possible with a one-handed weapon (or none)
    static const u8 ShieldGuard[10] = {0, 0, 3, 3, 3, 3, 3, 3, 3, 3};
    bool one_handed = !sword->present || strncmp(sword->name, "Weapon_Sword_", 13) == 0;
    if (shield->present && shield->value > 0 && one_handed) {
        out->left = IsLow(shield) ? TriggerEffectVibration(2, 3, 20) : TriggerEffectFeedback(ShieldGuard);
    }

    // Surfing: continuous light gravel-like rumble on both triggers
    if (state->surfing && (now >= state->surf_until_tick || state->bow_state != 0 || !shield->present)) {
        SetSurfing(state, false, now >= state->surf_until_tick ? "no shield wear for a while" : "bow or no shield");
    }
    if (state->surfing && g_botw_settings.surf_rumble_strength > 0) {
        out->right = TriggerEffectVibration(0, g_botw_settings.surf_rumble_strength, 50);
        out->left  = TriggerEffectVibration(0, g_botw_settings.surf_rumble_strength, 50);
    }

    // Haptic textures: a faint shake while the bow is drawn, gravel while surfing, a hum while Magnesis holds something
    HapticsSetLoop(HapticLoop_BowTension, state->bow_state == 2 && bow->present && arrow->present);
    HapticsSetLoop(HapticLoop_Surf, state->surfing);
    HapticsSetLoop(HapticLoop_Magnesis, state->magnesis_held);

    // Short event effects override the profile
    for (int t = 0; t < 2; ++t) {
        if (now < state->pulse_until_tick[t]) {
            if (t == 0) out->right = state->pulse_effect[t];
            else        out->left  = state->pulse_effect[t];
        }
    }

    return true;
}

u64 BotwGetPlayerActor(const DmntCheatProcessMetadata *meta) {
    u64 ptr = 0;
    if (R_FAILED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + PLAYER_INFO_OFFSET, &ptr, sizeof(ptr))) || ptr == 0 ||
        R_FAILED(dmntchtReadCheatProcessMemory(ptr + PLAYER_HEARTS_CHAIN1, &ptr, sizeof(ptr)))) {
        return 0;
    }
    return ptr;
}

// Heartbeat: two red flashes per second ("lub-dub") while health is low
bool BotwComputeLightbar(BotwState *state, u8 *r, u8 *g, u8 *b) {
    if (state->supported && armGetSystemTick() < state->flash_until_tick) {
        *r = 0xFF;
        *g = 0x50;
        *b = 0x00;
        return true;
    }

    if (!state->supported || state->hearts <= 0 || g_botw_settings.low_health_quarters == 0 ||
        (u32)state->hearts > g_botw_settings.low_health_quarters) {
        return false;
    }

    // On the title screen and while a save loads hearts read 1: only trust the value once the
    // game is loaded (something is equipped) and not right after a death or reload
    bool loaded = false;
    for (int t = 0; t < BotwSlot_Count; ++t) {
        loaded |= state->equipped[t].present;
    }
    if (!loaded || !state->hearts_trusted || armGetSystemTick() < state->settle_until_tick) {
        return false;
    }

    u64 total_ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    u64 ms = total_ms % 1000;
    bool flash = ms < 120 || (ms >= 250 && ms < 370);

    // Felt heartbeat in step with the lightbar: one "lub-dub" at the start of every second
    if (total_ms / 1000 != state->heartbeat_second) {
        state->heartbeat_second = total_ms / 1000;
        if (ms < 200 && !state->menu_open) {
            HapticsPlay(HapticPattern_Heartbeat, 1.0f);
        }
    }
    *r = flash ? 0x80 : 0x0C;
    *g = 0;
    *b = 0;
    return true;
}

void BotwUpdate(BotwState *state, const DmntCheatProcessMetadata *meta) {
    if (!state->supported) {
        return;
    }

    if (state->pmdm == 0) {
        u64 pmdm = 0;
        if (R_FAILED(dmntchtReadCheatProcessMemory(meta->main_nso_extents.base + PMDM_INSTANCE_OFFSET, &pmdm, sizeof(pmdm))) || pmdm == 0) {
            return;
        }
        state->pmdm = pmdm;
        LogLine("botw: PauseMenuDataMgr at %010lX", pmdm);
    }

    UpdatePlayer(state, meta);

    u64 now = armGetSystemTick();
    if (now >= state->next_scan_tick) {
        state->next_scan_tick = now + armNsToTicks(SCAN_INTERVAL_NS);
        state->player = BotwGetPlayerActor(meta);
        ScanPouch(state);
    }

    UpdateBow(state);
    UpdateActionFlags(state);
    UpdateRunes(state, meta);

    TrackValues(state);
}
