# CommonLibNV Plugin Template

A basic [NVSE](https://github.com/xNVSE/NVSE) plugin template for Fallout: New Vegas (1.4.0.525) using CommonLibNV. It exports `NVSEPlugin_Query` / `NVSEPlugin_Load`, reads a TOML settings file, writes a log, listens to NVSE messages and registers one script command.

### Requirements
* [XMake](https://xmake.io) [3.0.0+]
* MSVC (Visual Studio 2022) with the x86 toolset (the game is a 32-bit process)

## Getting Started
```bat
git clone --recurse-submodules https://github.com/frakkin64/commonlibnv-template.git
cd commonlibnv-template
```

`lib/commonlibnv` is the [CommonLibNV](https://github.com/frakkin64/commonlibnv) submodule (`.gitmodules` points at `https://github.com/frakkin64/commonlibnv.git`, branch `main`). If you cloned without `--recurse-submodules`, run `git submodule update --init`; to move to the latest CommonLibNV, run `git submodule update --remote lib/commonlibnv` and commit the new submodule pointer.

### Build
```bat
xmake f -p windows -a x86 -m releasedbg
xmake build
```

> ***Note:*** *This will generate a `build/windows/` directory in the **project's root directory** with the build output.*

### Build Output (Optional)
If you want to redirect the build output, set one of the following environment variables:

- Path to a Mod Manager mods folder: `XSE_FNV_MODS_PATH`

  or

- Path to a Fallout: New Vegas install folder: `XSE_FNV_GAME_PATH`

The plugin dll is copied to `Data/NVSE/Plugins`.

### Project Generation (Optional)
If you use Visual Studio, run the following command:
```bat
xmake project -k vsxmake
```

**Alternatively**, generate a `compile_commands.json` for a language server such as clangd:
```bat
xmake project -k compile_commands
```

## What is in here
| File | Purpose |
|---|---|
| `src/main.cpp` | `NVSE_PLUGIN_QUERY` (fills `PluginInfo`, checks the runtime) and `NVSE_PLUGIN_LOAD` (settings, `NVSE::Init`, message listener, command registration) |
| `src/Commands.cpp`, `include/Commands.h` | `TemplateHello [value]` (argument extraction with `ScriptInterface::ExtractArgsEx`) and `TemplateSelfTest`, which calls game code through the CommonLibNV definitions (`TESForm::GetFormByNumericID`, a virtual call, `TESObjectREFR` members) and logs `[ ok ]`/`[FAIL]` per check; the command returns the number of failed checks |
| `include/Settings.h` | a `REX::TOML` setting (`iLogLevel`); the file is `Data/NVSE/Plugins/template.toml` |
| `src/pch.h` | precompiled header: `RE/Fallout.h` (game definitions) and `NVSE/NVSE.h` (plugin API) |

The log is written to `Documents/My Games/FalloutNV/NVSE/<plugin name>.log`.

> ***Note:*** every plugin that adds script commands needs its own opcode range from the NVSE team. The template uses `0x2000`, the range NVSE reserves for testing.

### Upgrading Packages (Optional)
```bat
xmake repo --update
xmake require --upgrade
```
