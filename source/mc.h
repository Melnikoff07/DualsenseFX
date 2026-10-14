// Client for MissionControl's mc service (trigger effect commands added in our fork)
#pragma once
#include <switch.h>

// One trigger's adaptive effect block as sent to the DualSense (mode + 10 parameters)
typedef struct {
    u8 mode;
    u8 params[10];
} __attribute__((packed)) TriggerEffect;

typedef struct {
    TriggerEffect right;
    TriggerEffect left;
} __attribute__((packed)) TriggerEffects;

Result mcInitialize(void);
void mcExit(void);

Result mcGetBuildVersionString(char *out, size_t size);
Result mcSetTriggerEffects(const TriggerEffects *effects);
Result mcClearTriggerEffects(void);

typedef struct {
    u8 left;
    u8 right;
    u8 valid;
    u8 reserved;
} McAnalogTriggers;

Result mcGetAnalogTriggers(McAnalogTriggers *out);

typedef struct {
    u32 enabled;
    u32 thread_running;
    u32 ticks;
    u32 reports_sent;
    u32 reports_failed;
    u32 last_error;
    u32 prime_failed;   // feature reads 0x09/0x22 that failed at connect (bit 0 = 0x09, bit 1 = 0x22)
    u32 chirps;         // startup test buzzes
    u32 fallbacks;      // controllers that fell back to the simplified rumble
    u32 input_reports;  // 0x31 input reports in haptics mode
    u32 input_mic;      // of those, microphone audio reports (skipped)
    u32 input_other;    // input reports with other ids
    u8  last_input[8];  // first bytes of the last 0x31 input report
    u32 input_phase[4]; // 0x31 input reports per startup phase: connect, pause, test buzz, after the test
    u32 voices;         // pattern voices received
} McHapticsStatus;

Result mcGetHapticsStatus(McHapticsStatus *out);

// One voice of a haptic pattern for the DualSense voice coils (needs dualsense_hd_haptics in MissionControl):
// a sine or low-passed noise sweeping from freq_start to freq_end, with an attack / hold / decay envelope
typedef struct {
    u8  slot;           // 0-7: replaces the voice playing in this slot; amplitude 0 on both sides stops the slot
    u8  flags;          // McHapticFlag_*
    u8  amp_left;       // 0-255
    u8  amp_right;
    u16 freq_start;     // Hz: sine frequency, or noise low-pass cutoff
    u16 freq_end;
    u16 delay_ms;
    u16 attack_ms;
    u16 hold_ms;        // a looping voice holds until replaced or stopped
    u16 decay_ms;
} McHapticVoice;

enum {
    McHapticFlag_Noise = 0x01,
    McHapticFlag_Loop  = 0x02,
};

#define MC_HAPTIC_VOICES_MAX 4

Result mcPlayHaptics(const McHapticVoice *voices, u32 count);
Result mcStopHaptics(void);
Result mcSetLightbarColour(u8 r, u8 g, u8 b);
Result mcClearLightbarColour(void);

// Effect builders, same encodings as dualsense_controller.cpp (Nielk1's TriggerEffectGenerator)
TriggerEffect TriggerEffectOff(void);
TriggerEffect TriggerEffectSimpleFeedback(u8 start, u8 force);
TriggerEffect TriggerEffectFeedback(const u8 strengths[10]);
TriggerEffect TriggerEffectWeapon(u8 start, u8 end, u8 strength);
TriggerEffect TriggerEffectVibration(u8 start, u8 amplitude, u8 frequency);
