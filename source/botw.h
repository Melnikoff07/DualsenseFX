// BotW 1.6.0 game state readers (memory layout notes in botw.c)
#pragma once
#include <switch.h>
#include "dmntcht.h"
#include "mc.h"

typedef enum {
    BotwSlot_Sword  = 0,
    BotwSlot_Bow    = 1,
    BotwSlot_Arrow  = 2,
    BotwSlot_Shield = 3,
    BotwSlot_Count,
} BotwSlot;

typedef struct {
    bool present;
    u32 slot;       // index in the pouch item buffer
    s32 value;      // durability x100 for weapons, count for arrows
    char name[64];
} BotwEquippedItem;

typedef struct {
    bool supported;     // running build matches the offsets below
    u64 pmdm;           // PauseMenuDataMgr instance
    u64 next_scan_tick;
    BotwEquippedItem equipped[BotwSlot_Count];
    bool dumped[BotwSlot_Count];

    s32 hearts;             // quarter hearts, -1 if unreadable
    s32 last_hearts;
    bool hearts_trusted;    // false until the hearts value is known to be real (see UpdatePlayer)
    s32 hearts_at_attach;
    u64 player;             // player actor, refreshed every scan
    u32 bow_state;          // player +1F88: 0 none, 1 nocking, 2 drawn, 3 shot
    u64 bow_active_until_tick; // keep light bow tension shortly after a shot, for rapid fire
    u32 action_flags;       // player +C40
    u64 flash_until_tick;   // lightbar flash (Daruk's Protection)
    bool surfing;
    u64 surf_until_tick;    // surfing ends at this point unless the shield wears down again
    u64 hearts_changed_tick;
    bool menu_open;         // pause menu open (mEquippedWeapons is only filled then)
    u64 settle_until_tick;  // effects stay off until then (death, save reload)
    u64 pulse_until_tick[2];// short event effects: [0] right trigger, [1] left trigger
    TriggerEffect pulse_effect[2];
    bool effects_off_logged;
    float stamina;          // 1800 = one wheel, -1 unreadable
    u8 stamina_rising;      // polls in a row the stamina went up (refilling)
    u64 airborne_since_tick;
    u64 break_tick;         // last weapon break pattern, to play it once
    u64 heartbeat_second;   // last second a felt heartbeat was played

    // Runes (RuneMgr, see botw.c)
    bool runes_read;        // the values below are from a previous poll
    u32 rune_flags;
    u64 rune_target;
    u64 rune_lock_tick;     // last lock click, the target flickers at the edge of the aim
    u32 bomb_state[2];
    u32 stasis_flags;
    bool magnesis_held;
    u64 rune_mgr;
    u32 rune_selected;
    bool cryonis_event;     // event bit seen on the last fast poll

    u32 ground_material;    // player +1CA8
} BotwState;

// Tunables from config.ini
typedef struct {
    u8 shield_hit_strength; // 0-8, 0 disables the block impact pulse
    u8 shield_hit_style;    // 1 = trigger stiffens (push back), 2 = vibration
    u32 shield_hit_ms;
    u32 low_health_quarters; // heartbeat on the lightbar at or below this many quarter hearts, 0 disables
    u8 bow_release_strength; // 0-8, pulse on R2 when an arrow is shot, 0 disables (not felt: the finger is already off the trigger)
    u8 bow_drawn_strength;   // 1-8, R2 resistance once the bow is drawn (bow state 2)
    u8 surf_strength;        // 0-8, vibration on both triggers when the shield wears down while surfing, 0 disables
    u8 surf_rumble_strength; // 0-8, continuous light vibration on both triggers while surfing, 0 disables
    u32 surf_timeout_ms;     // surfing ends after this long without shield wear
    u8 daruk_strength;       // 0-8, vibration on both triggers when Daruk's Protection blocks a hit, 0 disables (lightbar flash stays)
    bool triggers;           // adaptive trigger effects at all
} BotwSettings;

extern BotwSettings g_botw_settings;

void BotwInit(BotwState *state, const DmntCheatProcessMetadata *meta);
bool BotwInGame(const BotwState *state);
void BotwUpdate(BotwState *state, const DmntCheatProcessMetadata *meta);

// Called as often as possible (~100 Hz): one-frame rune events that the 20 Hz update would miss (Cryonis)
void BotwPollFast(BotwState *state);

// Trigger effects for the current game state. Returns false if the build is not supported.
bool BotwComputeEffects(BotwState *state, TriggerEffects *out);

// Lightbar colour for the current game state. Returns false when the normal player colour should show.
bool BotwComputeLightbar(BotwState *state, u8 *r, u8 *g, u8 *b);

// Player actor (PlayerInfo::mPlayerActor), 0 if not available
u64 BotwGetPlayerActor(const DmntCheatProcessMetadata *meta);
