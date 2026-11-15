# NitroEdit LT1.0

A level editor for **LithTech 1.0** games (Blood 2, Shogo). It is the Jupiter-era
DEdit, adapted to read and write the LithTech 1.0 level and texture formats,
with a few quality-of-life additions.

## Building

Requirements: Visual Studio 2017 or newer (or the Build Tools) with the
"Desktop development with C++" workload and the component
"C++ MFC for latest v14x build tools (x86 & x64)".

From the repository root:

    tools\DEdit\build\build_dedit.bat            release build
    tools\DEdit\build\build_dedit.bat rebuild    recompile everything
    tools\DEdit\build\build_dedit.bat debug      debug build

The output is `build\dedit\<config>\NitroEdit.exe`. Use `rebuild` after changing
any header or resource file; a plain build only recompiles files without an
object file or with a newer source.

Build errors are written to `build\dedit\<config>\errors.txt`, the full log to
`build.log` next to it.

## Running a level

Process and Run work through `Processor.exe` from the game's own tools, which is
not included here. Put it next to `NitroEdit.exe` (or in the project's `tools`
folder). NitroEdit writes the level as an LT1 `.ed` file, then starts Processor
on it.

## What's different from stock DEdit

**Levels**
- Opens LithTech 1.0 `.ed` files with File > Import LithTech 1.0 World (.ed),
  including the compressed `.ed` files the games shipped with. Save the result
  as `.lta`/`.ltc`.
- Writes LT1 `.ed` files for Processor. Texture scale, rotation and offset are
  converted both ways.
- Object properties the project does not define for a class are kept, so they
  survive a round trip.

**Textures**
- Reads LT1 DTX files, shows their dimensions, and recognises the old model-skin
  layout (with a message instead of a load error).
- The textures you were using are selected again the next time you start.

**Terrain**
- Real-time painting: strokes update the view as you paint instead of on release.
- Fixed the dark outlines caused by coincident side and bottom faces.

**Controls and appearance**
- A **Hammer** control style (Options > Controls): middle-mouse pan, wheel zoom,
  free flight, and Hammer-like hotkeys.
- **Color Schemes** (Options > Color Schemes): DEdit (default), Hammer, Blueprint,
  Amber Terminal, Modolith, NitroEdit and Paper. Pick one and press Apply, or tick
  "Cycle through all schemes" to rotate them. If a build changes the colours of
  the scheme you are using, they are re-applied at startup.

## Settings

Settings live under `HKEY_CURRENT_USER\Software\LithTech Inc.\Jupiter\NitroEdit`.
On first launch, anything saved by the older DEdit build (`...\Jupiter\DEdit`) is
copied across. The old key is left untouched.

## Known limits

- Prefab references are not written to `.ed` files.
- Per-polygon flags that LT1 `.ed` files carry beyond the texture mapping are not
  kept on export, apart from the texture-origin anchor, which is folded into the
  texture offset.
- Project and world file types are still registered under their DEdit names
  (`.dep`, `DEdit.Document`) so existing associations keep working.

## Credits and licence

NitroEdit is based on DEdit from the Jupiter engine source release by Touchdown
Entertainment (copyright 1998-2006), with portions copyright Micro Focus Inc.
It is used under the terms of that release; see the repository's licence files.
