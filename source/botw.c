// BotW 1.6.0 (build id 8E9978D50BDD20B4) game state readers.
//
// Inventory: uking::ui::PauseMenuDataMgr (decomp: src/Game/UI/uiPauseMenuDataMgr.h, v1.5.0).
//  - main+2CA6D48 holds the instance pointer (from switch-cheats-db 1.6.0 codes; matches mEquippedWeapons at +447D8).
//  - mEquippedWeapons[4] is only filled while the pause menu is open, so instead we scan the pouch item
//    buffer (SafeArray<PouchItem, 420>, item size 0x298) for items with mEquipped set.
//  - Buffer start mgr+0x90 was derived from item pointers seen in the log (all at 0x90 mod 0x298).
//  - Field offsets are the decomp 1.5.0 PouchItem offsets + 8, which is what the cheat codes use
//    (value at +0x20, name chars at +0x3C). NOT fully verified yet: the scanner logs raw bytes to check.
#include <stdio.h>
#include <string.h>
#include "botw.h"

void LogLine(const char *fmt, ...);

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

#define SCAN_INTERVAL_NS       (1000 * 1000000ULL)
#define SCAN_CHUNK_ITEMS       12

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
        LogLine("  %s +%02lX: %s", label, off, line);
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
                LogLine("botw: second equipped %s at slot %u", SlotNames[type], first + i);
                continue;
            }
            e->present = true;
            e->slot = first + i;
            memcpy(&e->value, item + ITEM_VALUE_OFFSET, sizeof(e->value));
            CopyName(e->name, item + ITEM_NAME_OFFSET);

            if (!state->dumped[type]) {
                state->dumped[type] = true;
                LogLine("botw: raw %s item, slot %u:", SlotNames[type], first + i);
                LogHex(SlotNames[type], item, ITEM_HEADER_SIZE);
            }
        }
    }

    for (int t = 0; t < BotwSlot_Count; ++t) {
        BotwEquippedItem *old = &state->equipped[t];
        BotwEquippedItem *now = &found[t];
        if (old->present != now->present || old->slot != now->slot || strcmp(old->name, now->name) != 0) {
            if (now->present) {
                LogLine("botw: equipped %-6s slot %3u %-24s value %d (%u items in pouch)", SlotNames[t], now->slot, now->name, now->value, valid_items);
            } else {
                LogLine("botw: equipped %-6s none", SlotNames[t]);
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
        if (R_SUCCEEDED(dmntchtReadCheatProcessMemory(addr, &value, sizeof(value))) && value != e->value) {
            LogLine("botw: %-6s value %d -> %d", SlotNames[t], e->value, value);
            e->value = value;
        }
    }
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

    u64 now = armGetSystemTick();
    if (now >= state->next_scan_tick) {
        state->next_scan_tick = now + armNsToTicks(SCAN_INTERVAL_NS);
        ScanPouch(state);
    }

    TrackValues(state);
}
