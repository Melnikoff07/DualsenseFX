# DualsenseFX for Zelda: Breath of the Wild

DualSense adaptive triggers, lightbar effects and HD haptics that react to what happens in
The Legend of Zelda: Breath of the Wild **1.6.0** (build ID `8E9978D50BDD20B4`) on a Switch running Atmosphère.

The haptics come from two sources: the game's state (hearts, stamina, the bow, the runes) and the game's own sounds.
Every short sound the game plays is looked up by name, so a footstep tap lands exactly when the game plays the
step, with a texture for the ground under Link's feet. Everything can be tuned while playing from an Ultrahand
overlay, without a reboot.

| Part | Location on the SD card | What it does |
|---|---|---|
| [MissionControl fork](MissionControl) (submodule, branch `dualsense-triggers`) | `atmosphere/contents/010000000000bd00` | DualSense driver with HD haptics over Bluetooth; extra `mc` service commands: 6/7 trigger effects, 8 analog trigger state, 9/10 lightbar colour, 11 haptics status, 12/13 haptic pattern voices, 14 controller input. Overrides are cleared automatically when the client session closes |
| `adaptive_triggers` sysmodule | `atmosphere/contents/42000000000d5e00` | detects BotW, reads game state through `dmnt:cht` (read only) and sends effects to MissionControl |
| DualsenseFX overlay | `switch/.overlays/dualsense_fx.ovl` | Tesla / Ultrahand menu: effects on / off and strength per group |

## Requirements

- A Switch running Atmosphère (tested: firmware 22.5.0, Atmosphère 1.12.0, emuMMC).
- A Sony DualSense paired through MissionControl. The fork replaces the stock MissionControl 0.16.0.
- Breath of the Wild **1.6.0** (other versions are ignored by the module).
- For the overlay: Ultrahand Overlay or Tesla (nx-ovlloader).

Use it offline on emuMMC. It reads game memory through Atmosphère's cheat service the same way Breeze/EdiZon do and
never writes to it.

## Install

1. Back up your current `atmosphere/contents/010000000000bd00` (stock MissionControl).
2. Download the zip from the latest [release](../../releases) and extract it to the root of the SD card.
3. HD haptics are off by default. To turn them on, create `config/MissionControl/missioncontrol.ini` (or add to it):
   ```
   [misc]
   dualsense_hd_haptics=true
   ; strength in percent, 0-400
   dualsense_haptics_gain=200
   ```
4. Reboot and reconnect the DualSense. About 2 s after it connects it plays a short rising buzz: haptics work.

To go back to the normal rumble, delete `missioncontrol.ini` and reboot.

## Settings

All files are in `config/adaptive_triggers/` and are applied within a second after they change, no reboot needed:

| File | What |
|---|---|
| `config.ini` | effects on / off, strength per group (combat, body, runes, world, footsteps), trigger tuning. The overlay edits this file |
| `patterns.ini` | the shape of every haptic pattern: up to 4 voices (sine or noise, amplitude per hand, frequency sweep, delay / attack / hold / decay). Written with the defaults on first start |
| `sounds.ini` | which game sounds play which pattern: `mask = pattern [strength %]`, `*` matches anything, first match wins. Written with the defaults on first start |

The overlay (**DualsenseFX** in the Ultrahand list) has switches for the adaptive triggers, the haptics and the
low-health heartbeat, and strength sliders (off, 25 % … 200 %) for every group. Log: `config/adaptive_triggers/log.txt`
(previous boot: `log.old.txt`).

## Effects

### Adaptive triggers and lightbar

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

### HD haptics

With HD haptics on, the game's HD rumble plays on the voice coils (weak effects boosted, peaks soft clipped) and
the module adds its own patterns, mixed with it. Left / right put an effect in the hand that would feel it.

| Group | Event | Pattern |
|---|---|---|
| combat | hit with a one-handed / two-handed weapon / spear | crisp knock / deep thud / short sharp poke |
| combat | blocked hit on the shield | heavy knock in the left hand |
| combat | arrow nocked / bow drawn / arrow shot | tiny click / faint shaking that builds up / string snap with recoil |
| combat | weapon or shield breaks, Daruk's Protection | crunch with falling pieces, falling shockwave |
| body | damage / healing | rough hit scaled by the hearts lost / soft swell |
| body | 1 heart or less | felt heartbeat in step with the red lightbar |
| body | landing (from the game's landing sound) | soft tap after a hop, thud after a real fall |
| body | stamina runs out / refills | three fading pulses / barely felt calm texture |
| body | shield surfing | gravel texture plus bumps |
| runes | round and cube bombs | pop when it appears, heavy blast when it explodes |
| runes | Magnesis | rising swell, click when it locks on metal, clunk on grab, hum while holding, release |
| runes | Stasis | crack on freeze, ticks in step with the frozen target's flashing (faster towards the end), release |
| runes | Cryonis | grinding ice with a crack when the pillar stands, shatter when it breaks |
| world | water | splash when entering, strokes while swimming, splashy steps in shallow water |
| world | chests, items, rolls, jumps, climbing, crates | lid, small sparkle, roll, push off, grip, crack |
| footsteps | every step (heel and lighter toe) | texture for the ground: grass, sand, soil, stone, wood, metal, ice, snow, cloth, water; stronger when running |

## How the sound driven haptics work

BotW uses NintendoWare audio (`nn::atk`, its RTTI type names are kept in the binary). `[main+2CC1670]` is the
`SoundArchivePlayer`; its `+0x128` points to a pool of 64 `WaveSound` objects (`0x3A0` bytes each, vtable
`main+2C17828`, checked before use). A slot's `+0x104` changes with every new sound and `+0x200` points to the FWAV
being played. BotW keeps its sounds in BARS packages; the package's AMTA metadata has the sound's name, for example
`Mt_Grass_Pl_MoveFast_HeelL00` (ground, player, pace, heel or toe, foot) or `Water_In_SmallSlow01`. The module polls
the pool 100 times a second, looks every new FWAV up once and plays what `sounds.ini` says.

To add an effect for another sound: turn on the network control (below), run `tools/sndlog.py log.txt 120` while doing
the thing in the game, find its name in the log and add a line to `sounds.ini`, for example `Pl_Get* = item get 80`.

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
| action flags | actor `+0xC40`: `0x18` ZL stance, `0x40000000` bow aim, `0xC000` Daruk's Protection blocked a hit, `0x2` jump / fall and also sprinting |
| ground material | actor `+0x1CA8`: 0 Undefined, 1 Soil, 2 Stone, 3 Sand, 4 Metal, 5 WireNet, 6 Grass, 7 Wood, 8 Water, 9 Snow, 10 Ice … (enum string in rodata) |
| world position | actor `+0x430` / `+0x434` / `+0x438` |
| Magnesis holding | actor `+0xD44` reads `Item_Magnetglove` |
| rune manager | `[main+2C9A668]` (found in the code: passed to the bomb spawn function). `+0x90` flags (`0x10` aiming, `0x4` / `0x8` bomb out), `+0x94` one-frame events (Cryonis `0x4`, with `+0x168` = 1 when a pillar is made), `+0x240` selected rune (0 bomb, 1 cube bomb, 2 Magnesis, 3 Stasis, 4 Cryonis), `+0x2D0` / `+0x2F8` bomb state (3 out, `0xE` exploding), `+0x410` target lock, `+0x488` Stasis (`0x10000` frozen, `0x100` flash) |
| game data flags | `[[[main+2CC5FE8]+0xBD8]]`: `+0x18` count and `+0x20` array of s32 flags (value `+0xC`, CRC32 of the name `+0x1C`, sorted by hash) |
| shield surfing | no flag found: starts when the ZL stance drops straight to 0 without the shield-lowering animation; also shield wear without the stance |

Sources: the [zeldaret/botw](https://github.com/zeldaret/botw) decompilation (v1.5.0), switch-cheats-db codes for this
build ID, the module's correlation searches, recordings over the network control and the game's own RTTI and
strings.

## Development: network control

`net=1` and a `net_token` (any long random string) in `config.ini` start a small TCP server (port 7330) for
development on the home network: read game memory, see controller input, play patterns and trigger effects live,
move the project's files and reboot. It never writes game memory and only touches the project's folders.

```
export SWNET_HOST=<console ip> SWNET_TOKEN=<net_token>
tools/swnet.py cmd help
tools/swnet.py cmd "pattern bomb explode"
tools/swnet.py read rune+0 1000 rune.bin
tools/sndlog.py sounds.txt 120
```

`config.ini` also has search modes that log memory changing with in-game actions (used to find the fields above).

## Building

Clone with submodules (the MissionControl fork with its own `libnx` / `Atmosphere-libs` submodules, and libtesla):

```
git clone --recurse-submodules https://github.com/Melnikoff07/DualsenseFX-for-Zelda-Breath-of-the-Wild.git
cd DualsenseFX-for-Zelda-Breath-of-the-Wild
```

Build the toolchain image once (devkitPro devkitA64 + the libnx revision MissionControl expects), then the parts:

```
docker build -t mc-devkit -f tools/Dockerfile MissionControl/lib/libnx
tools/dk 'cd MissionControl && make dist -j8'
tools/dk 'make -j8 dist'
tools/dk 'cd overlay && make -j8'
tools/package.sh 0.3.0          # release zip
```

`tools/dk` runs the command in that image with the repository mounted at `/work`. Without a local Docker it uses
a [Lima](https://lima-vm.io) VM (`LIMA_INSTANCE`, default `mcbuild`, created from `template:docker`) that mounts
the repository at `/work` — mount it at a path without spaces, the devkitPro makefiles do not handle them.

## Install from a build and roll back

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
- Parries have no pattern yet. Combat still uses durability and bow state; moving it to the game's sounds (hit
  material, arrow hits) is planned.

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
- Overlay: [libtesla](https://github.com/WerWolv/libtesla) by WerWolv.
