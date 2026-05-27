# CLAUDE.md

Guidance for working in this repository.

## What this is

TweakPNG is a low-level Win32 GUI utility (C++) for viewing and editing the chunk
structure of PNG files. This repo is a fork (origin: `jsummers/tweakpng`) whose focus is
**modernizing the UI for Windows 10/11** — dark mode title bar, menu bar, list view, and
dialogs; PerMonitorV2 DPI awareness; rounded corners and Mica backdrop.

It is a pure Win32 application: no framework, direct `WndProc` message handling, GDI
drawing, and the Windows common controls (`ListView`, `Header`, dialogs from `.rc`).

## Build

Visual Studio 2022 (or Build Tools). The solution is `proj/vs2022/tweakpng.sln`.
Configurations: `Debug`/`Release` × `Win32`/`x64`.

MSBuild is not on PATH in this environment. Use the full path:

```powershell
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild "D:\tools\tweakpng\proj\vs2022\tweakpng.sln" /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Output: `Release64\tweakpng.exe` (x64 Release). There is no test suite; verify changes by
running the app and inspecting the UI.

### Dependencies

The default build links `libpng` and `zlib`, expected as **sibling directories** of the
repo root (`..\..\..\libpng`, `..\..\..\zlib`) with prebuilt `.lib`s under
`Debug32`/`Release32`/`Debug64`/`Release64`. Also links `comctl32.lib` and `uxtheme.lib`.

Feature flags live in `twpng-config.h`:
- `TWPNG_SUPPORT_VIEWER` — image viewer (needs libpng + zlib)
- `TWPNG_USE_ZLIB` — compression (needs zlib); enables editing compressed text chunks
- Comment both out for a no-dependency build.

`WINVER`/`_WIN32_WINNT` are `0x0A00` (Windows 10+).

## Source layout

- `tweakpng.cpp` — entry point, main window `WndProcMain`, menu/command handling, dark mode setup, layout. Largest file; most UI work happens here.
- `chunk.cpp` — PNG chunk parsing/editing and the per-chunk-type edit dialogs.
- `viewer.cpp` — image viewer window (decodes via libpng).
- `iccprof.cpp` — ICC profile (iCCP) handling.
- `pngtodib.cpp` / `pngtodib.h` — PNG→DIB conversion for the viewer.
- `charset.cpp` — text/charset helpers.
- `tweakpng.h` — shared structs (notably `globals_struct`), prototypes.
- `tweakpng.rc` / `resource.h` — menus (`MENUMAIN`), dialogs, icons, accelerators.
- `twpng-config.h` — build-time feature flags.

## Dark mode notes (where the fragile bits are)

Windows dark mode for classic Win32 relies on **undocumented APIs and message layouts**.
When touching this, the canonical reference is `adzm/win32-custom-menubar-aero-theme` and
`ysc3839/win32-darkmode`.

- App-level dark mode: `SetPreferredAppMode(2)` and `AllowDarkModeForWindow` via
  `uxtheme.dll` ordinals (133/135/136). Title bar/corners/backdrop via
  `DwmSetWindowAttribute` (attrs 20/33/38) in `twpng_ApplyModernWindowStyle`.
- **Menu bar** is owner-drawn through the undocumented `WM_UAHDRAWMENU` (0x91) /
  `WM_UAHDRAWMENUITEM` (0x92). The `UAHDRAWMENUITEM` struct has `DRAWITEMSTRUCT` as its
  **first** member — get the layout wrong and nothing draws. The 1px light line Windows
  leaves under the dark menu bar is painted over in `WM_NCPAINT`/`WM_NCACTIVATE`.
- **ListView header** text color is not handled reliably by `DarkMode_ItemsView`. The
  header notifies its parent (the list view, not the main window), so the list view is
  **subclassed** (`MainListSubclassProc`) to intercept `NM_CUSTOMDRAW` and owner-draw
  header items.
- Dialogs: `twpng_InitDarkDialog` + `twpng_HandleDlgDarkMsg` apply theming and handle
  `WM_CTLCOLOR*`.

The main window has a real menu (`MENUMAIN`); commands are dispatched in `WM_COMMAND` and
enabled/disabled in `WM_INITMENU`. There is no separate toolbar.

## Conventions

- TCHAR / `_T()` throughout; build can be Unicode or non-Unicode (non-Unicode drops iTXt).
- Match the existing terse Win32 style. Keep comments to non-obvious "why" only.
