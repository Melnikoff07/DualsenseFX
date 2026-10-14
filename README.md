# adaptive_triggers

DualSense adaptive triggers and lightbar effects that react to what happens in
The Legend of Zelda: Breath of the Wild **1.6.0** (build ID `8E9978D50BDD20B4`) on a Switch running Atmosphère.

Two parts:

| Part | Location on the SD card | What it does |
|---|---|---|
| [MissionControl fork](MissionControl) (submodule, branch `dualsense-triggers`) | `atmosphere/contents/010000000000bd00` | DualSense driver; extra `mc` service commands: 6/7 trigger effects, 8 analog trigger state, 9/10 lightbar colour, 11 HD haptics status. Overrides are cleared automatically when the client session closes |
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

## Game memory used (1.6.0, verified on console)

| What | Where |
|---|---|
| inventory manager `PauseMenuDataMgr` | `[main+2CA6D48]` |
| inventory items | `mgr+0x90`, 420 slots of `0x298`; `+0x18` type, `+0x20` value (durability ×100), `+0x24` equipped, `+0x3C` name (decomp 1.5.0 layout + 8: 1.6.0 has a vtable) |
| pause menu open | `mgr+0x447D8` (`mEquippedWeapons`) non-null |
| player actor | `[[main+2CA1A78]+0x80]` (`PlayerInfo::mPlayerActor`) |
| hearts | actor `+0x848` (quarter hearts) |
| bow | actor `+0x1F88`: 0 none, 1 nocking, 2 drawn, 3 shot |
| action flags | actor `+0xC40`: `0x18` ZL stance, `0x40000000` bow aim, `0xC000` Daruk's Protection blocked a hit, `0x2` jump/fall |
| shield surfing | no flag found: starts when the ZL stance drops straight to 0 without the shield-lowering animation; also shield wear without the stance |

Sources: the [zeldaret/botw](https://github.com/zeldaret/botw) decompilation (v1.5.0), switch-cheats-db codes for this
build ID, the module's correlation search and watch logs.

## Building

Clone with submodules (the MissionControl fork and its own `libnx` / `Atmosphere-libs` submodules):

```
git clone --recurse-submodules https://github.com/__OWNER__/botw-dualsense-triggers.git
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

- **HD haptics over Bluetooth do not work on the Switch.** The MissionControl fork contains an experimental
  `dualsense_hd_haptics` option that synthesises the game's HD rumble and streams it to the DualSense voice coils
  in output report `0x32`. In testing the controller never played the stream, Bluetooth writes started failing
  with `2113-6100` after about two minutes, and suspending the game froze the console. It is off by default;
  do not enable it.

## Credits

- [zeldaret/botw](https://github.com/zeldaret/botw) decompilation.
- Trigger effect encodings: Nielk1's TriggerEffectGenerator (used by DSX).
- DualSense Bluetooth haptics report format: [SAxense](https://github.com/egormanga/SAxense) by egormanga
  ([apps.sdore.me/SAxense](https://apps.sdore.me/SAxense)), cross-checked with
  [awalol/dualsense-bt-haptics](https://github.com/awalol/dualsense-bt-haptics).
- [MissionControl](https://github.com/ndeadly/MissionControl) by ndeadly.
