# adaptive_triggers

DualSense adaptive triggers, lightbar effects and HD haptics that react to what happens in
The Legend of Zelda: Breath of the Wild **1.6.0** (build ID `8E9978D50BDD20B4`) on a Switch running Atmosphère.

Two parts:

| Part | Location on the SD card | What it does |
|---|---|---|
| [MissionControl fork](MissionControl) (submodule, branch `dualsense-triggers`) | `atmosphere/contents/010000000000bd00` | DualSense driver with HD haptics over Bluetooth; extra `mc` service commands: 6/7 trigger effects, 8 analog trigger state, 9/10 lightbar colour, 11 haptics status, 12/13 haptic pattern voices. Overrides are cleared automatically when the client session closes |
| `adaptive_triggers` sysmodule | `atmosphere/contents/42000000000d5e00` | detects BotW, reads game state through `dmnt:cht` and sends effects to MissionControl |

## Requirements

- A Switch running Atmosphère (tested: firmware 22.5.0, Atmosphère 1.12.0, emuMMC).
- A Sony DualSense paired through MissionControl. The fork replaces the stock MissionControl 0.16.0.
- Breath of the Wild **1.6.0** (other versions are ignored by the module).

Use it offline on emuMMC. It reads game memory through Atmosphère's cheat service the same way Breeze/EdiZon do.

## Quick install

Download the zip from the latest [release](../../releases), back up your `atmosphere/contents/010000000000bd00`,
and extract the zip to the root of the SD card. Reboot. Settings are in `config/adaptive_triggers/config.ini`.

## Effects

| Where | Event | Feel |
|---|---|---|
| R2 | bow drawn | tight tension; free until an arrow is nocked; stays light for 1.5 s after a shot for rapid fire |
| R2 | bow with 3 shots left / sword hit with 3 hits left | weak buzz / pulse |
| L2 | shield equipped with a one-handed weapon (or none) | even resistance |
| L2 | hit on a raised shield | short, light vibration |
| both | Daruk's Protection blocks a hit | vibration + orange lightbar flash |
| both | shield surfing | continuous light rumble, stronger pulse when the shield wears down |
| lightbar | 1 heart or less | red heartbeat |
| — | pause menu, death, save loading, title screen | everything off |

Settings: `config/adaptive_triggers/config.ini` on the SD card (commented). Log: `config/adaptive_triggers/log.txt`
(previous boot: `log.old.txt`).

## HD haptics (experimental)

The MissionControl fork can drive the DualSense voice coils over Bluetooth: the game's HD rumble is played as
haptics instead of the simplified two-motor rumble, and the module adds its own patterns for game events. It is off by
default. To turn it on, create `config/MissionControl/missioncontrol.ini` on the SD card (or add to it):

```
[misc]
dualsense_hd_haptics=true
; strength in percent, 0-400
dualsense_haptics_gain=200
```

Reboot and reconnect the controller: about 2 s after it connects it plays a short rising buzz, which means the stream
works. Pattern strength: `haptics_strength` in `config/adaptive_triggers/config.ini` (`haptics=0` turns the patterns
off and keeps the game's rumble).

| Event | Pattern |
|---|---|
| hit with a one-handed weapon | crisp knock with a short recoil, mostly in the right hand |
| hit with a two-handed weapon | deep, heavy thud |
| hit with a spear | short sharp poke |
| blocked hit on the shield | heavy knock in the left hand |
| arrow nocked / bow drawn / arrow shot | tiny click / faint shaking that slowly builds up / string snap with recoil into the bow hand |
| damage | rough hit, stronger the more hearts were lost |
| healing | soft swell |
| 1 heart or less | felt heartbeat in step with the red lightbar |
| Daruk's Protection | falling shockwave |
| weapon or shield breaks | crunch with falling pieces |
| landing after a fall or a glide | dull thud, stronger after longer falls |
| shield surfing | gravel texture plus bumps when the shield wears down |
| stamina runs out | three fading pulses |

How it works: the Switch's Bluetooth stack does not deliver the DualSense's 142-byte haptics report `0x32` intact (and
streaming it froze the console in testing), so the audio packets are sent inside the 78-byte report `0x31`, with
the stream control packet cut down to its essential fields. The controller must never see the rumble emulation flags,
and it is armed like [DS5Dongle](https://github.com/awalol/DS5Dongle) does (feature reads `0x09`, `0x20`, `0x22`,
`0x05`, audio path enabled). The details and the failed attempts are in the fork's commit history.

## Game memory used (1.6.0, verified on console)

| What | Where |
|---|---|
| inventory manager `PauseMenuDataMgr` | `[main+2CA6D48]` |
| inventory items | `mgr+0x90`, 420 slots of `0x298`; `+0x18` type, `+0x20` value (durability ×100), `+0x24` equipped, `+0x3C` name (decomp 1.5.0 layout + 8: 1.6.0 has a vtable) |
| pause menu open | `mgr+0x447D8` (`mEquippedWeapons`) non-null |
| player actor | `[[main+2CA1A78]+0x80]` (`PlayerInfo::mPlayerActor`) |
| hearts | actor `+0x848` (quarter hearts) |
| stamina | `[[main+2C9FD70]+0x38]+0x2AC` (f32, 1800 = one wheel) |
| bow | actor `+0x1F88`: 0 none, 1 nocking, 2 drawn, 3 shot |
| action flags | actor `+0xC40`: `0x18` ZL stance, `0x40000000` bow aim, `0xC000` Daruk's Protection blocked a hit, `0x2` jump/fall |
| shield surfing | no flag found: starts when the ZL stance drops straight to 0 without the shield-lowering animation; also shield wear without the stance |

Sources: the [zeldaret/botw](https://github.com/zeldaret/botw) decompilation (v1.5.0), switch-cheats-db codes for this
build ID, the module's correlation search and watch logs.

## Building

Clone with submodules (the MissionControl fork and its own `libnx` / `Atmosphere-libs` submodules):

```
git clone --recurse-submodules https://github.com/Melnikoff07/botw-dualsense-triggers.git
cd botw-dualsense-triggers
```

Build the toolchain image once (devkitPro devkitA64 + the libnx revision MissionControl expects), then build both parts:

```
docker build -t mc-devkit -f tools/Dockerfile MissionControl/lib/libnx
tools/dk 'cd MissionControl && make dist -j8'
tools/dk 'make -j8 dist'
```

`tools/dk` runs the command in that image with the repository mounted at `/work`. Without a local Docker it uses
a [Lima](https://lima-vm.io) VM (`LIMA_INSTANCE`, default `mcbuild`, created from `template:docker`) that mounts
the repository at `/work` — mount it at a path without spaces, the devkitPro makefiles do not handle them.

## Install and roll back

**Back up your current `atmosphere/contents/010000000000bd00` first** (for example into `backups/MissionControl-original/`).

With the SD card mounted (`SD=/Volumes/NS_SAMSUNG` by default):

```
tools/deploy.sh             # MissionControl fork + adaptive_triggers + config.ini
tools/deploy.sh --rollback  # restore the backed up MissionControl, remove adaptive_triggers
```

To disable only the module, delete `atmosphere/contents/42000000000d5e00/flags/boot2.flag`, or toggle it in an
overlay sysmodule manager (the module ships a `toolbox.json`).

## Limitations

- BotW 1.6.0 only. Other builds are detected and left alone ("unknown build" in the log).
- While BotW runs the module holds one of the two `dmnt:cht` sessions: Breeze or EdiZon work, but not both at once.
- Attaches 30 s after launch: attaching earlier conflicts with SaltyNX injection.

## Known issues

- HD haptics are experimental: tested on one console and one controller. If a Bluetooth write fails the controller
  falls back to the simplified rumble until it reconnects. Earlier attempts (report `0x32`) froze the console;
  if anything goes wrong, delete `config/MissionControl/missioncontrol.ini` and reboot.
- Bomb explosions and parries have no pattern yet: nothing in game memory that marks them has been found.

## Credits

- [zeldaret/botw](https://github.com/zeldaret/botw) decompilation.
- Trigger effect encodings: Nielk1's TriggerEffectGenerator (used by DSX).
- DualSense Bluetooth haptics report format: [SAxense](https://github.com/egormanga/SAxense) by egormanga
  ([apps.sdore.me/SAxense](https://apps.sdore.me/SAxense)), cross-checked with
  [awalol/dualsense-bt-haptics](https://github.com/awalol/dualsense-bt-haptics).
- [MissionControl](https://github.com/ndeadly/MissionControl) by ndeadly.
- HD haptics over Bluetooth: [DS5Dongle](https://github.com/awalol/DS5Dongle) by awalol (packet layout, controller
  arming, field notes), xlanor's [MissionControl fork](https://github.com/xlanor/MissionControl/tree/akira) notes on
  rumble emulation latching the voice coils.
