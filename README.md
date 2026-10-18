# RealtimeVatsNV

NVSE plugin for Fallout: New Vegas (1.4.0.525) built on CommonLibNV: a real-time, lock-on VATS (no time freeze, mobility while locked, per-frame hit chance, target cycling).

Current state: lock-on key (X), target cycling (C), limb cycling (Z), crit arming (F), optional backwards cycling for both (`iKeyPrevious`, `iKeyLimbPrevious`), line-of-sight filtering (only visible hostiles are locked, cycled to or chained to; a target out of sight for 1.5 s is released), a minimum hit chance to lock (`fMinChanceToLock`, default 10%) and a 2500-unit range, shots the player cannot pay AP for are unassisted and free, the overlay shows the aimed limb, the target's DT/DR and a critical meter bar, per-frame hit chance from the engine's VATS formula, and aim assist for the player's projectiles through the engine's auto-aim. Keys, range, angle, limb and the chance cap are in `Data/NVSE/Plugins/RealtimeVatsNV.toml`.

### Requirements
* [XMake](https://xmake.io) [3.0.0+]
* MSVC (Visual Studio 2022) with the x86 toolset

### Build
```bat
xmake f -p windows -a x86 -m releasedbg
xmake build
```

Set `XSE_FNV_MODS_PATH` or `XSE_FNV_GAME_PATH` to copy the dll to `Data/NVSE/Plugins`.
