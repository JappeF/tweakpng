# CLAUDE.md

Guidance for working in this repository.

## What this is

TweakPNG is a low-level Win32 GUI utility (C++) for viewing and editing the chunk
structure of PNG files. This repo is a fork (origin: `jsummers/tweakpng`) with two themes:

1. **Modernizing the UI for Windows 10/11** — dark title bar, menu bar, list view, all
   dialogs, message boxes, and the color picker; PerMonitorV2 DPI awareness; rounded
   corners and Mica backdrop.
2. **Quality-of-life features for AI-generated images** (Automatic1111 / Stable
   Diffusion), centered on the `parameters` tEXt chunk.

It is a pure Win32 application: no framework, direct `WndProc` message handling, GDI
drawing, and the Windows common controls (`ListView`, `Header`, dialogs from `.rc`).
The fork version is `TWEAKPNG_FORK_VER_STRING` in `tweakpng.h` (shown in the About box).

## Build

Visual Studio 2022 (or Build Tools). The solution is `proj/vs2022/tweakpng.sln`.
Configurations: `Debug`/`Release` × `Win32`/`x64`.

MSBuild is not on PATH in this environment. Use the full path:

```powershell
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild "proj\vs2022\tweakpng.sln" /p:Configuration=Release /p:Platform=x64 /v:minimal
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
  `WM_CTLCOLOR*`. `twpng_InitDarkDialog` repaints with `RDW_ALLCHILDREN` so freshly themed
  child controls don't keep stale light-theme text.
- **Radio buttons** render their label black even when themed, so they're owner-drawn by
  `RadioSubclassProc` (themed dark glyph via `DrawThemeBackground` + light label). Edit
  controls get a flat dark border via `EditBorderSubclassProc` (the sunken client edge is
  painted over, not removed, to keep single-line text centered).
- **Message boxes**: the OS `MessageBox` can't be fully darkened (it paints a light footer
  in `WM_PAINT`), so `twpng_MessageBox` is a custom dark dialog (`DLG_MSGBOX`) used by
  `mesg()` and every other call site. Supports MB_OK/OKCANCEL/YESNO/YESNOCANCEL + icons.
- **System common dialogs** (the `ChooseColor` picker) are dark-themed via a scoped
  `WH_CBT` hook in `twpng_ChooseColorDark`, which themes the chrome on activation while
  leaving owner-drawn swatches/spectrum colorful.

The main window has a real menu (`MENUMAIN`); commands are dispatched in `WM_COMMAND` and
enabled/disabled in `WM_INITMENU`. There is no separate toolbar.

## Modeless editors

Two windows run modeless and **unowned** (so they minimize independently of the main
window and get their own taskbar buttons), one instance each:

- the tEXt/zTXt/iTXt editor (`DlgProcEdit_tEXt`, opened from `Chunk::edit()`), and
- the AI parameters viewer (`DlgProcAIParams` / `twpng_ViewAIParams`).

The main message loop routes their messages via `IsDialogMessage`
(`twpng_IsModelessEditorMessage` for the editor; a direct `g_hwndAIParams` check for the
viewer). Both are force-closed when the document is torn down — `Png::~Png` calls
`twpng_CloseModelessEditors()` and `twpng_CloseAIParamsView()` — and free their
heap context in `WM_NCDESTROY`. The tEXt editor edits live chunk data, so deleting its
chunk also closes it (`twpng_CloseEditorForChunk`); the AI viewer holds a copy, so it is
safe regardless.

## A1111 / AI image features

For PNGs, all keyed off text chunks (mainly tEXt keyword `parameters`); for JPEG/WebP,
keyed off the EXIF UserComment tag (see "Foreign image formats" below).

- **Auto-open** (`twpng_AutoOpenParamsText`, end of `OpenPngByName`): if the Preferences
  option `open_params_on_load` is set (default on), opening an image with a `parameters`
  chunk pops up the parsed viewer. The option persists in the registry (`open_params`).
  The foreign (JPEG/WebP) path triggers the same auto-open from its branch in
  `OpenPngByName`.
- **Parsed viewer** (`DLG_AIPARAMS`): splits the A1111 string into Prompt / Negative
  prompt / Settings with copy buttons. Parser `twpng_ParseA1111` splits on the
  `Negative prompt:` and `Steps:` line markers; `twpng_SettingsToLines` reformats the
  settings one-per-line while respecting double-quoted values (e.g. `Lora hashes: "..."`).
- **ComfyUI parser** (`twpng_ParseComfyJson`): ComfyUI stores its graph as JSON in
  a `prompt` tEXt chunk or EXIF UserComment; this walks the node graph with brace-aware
  substring scans (not a full JSON parser) and synthesizes an A1111-style string the
  parsed viewer can consume.
- **View AI Parameters** (`twpng_ViewAIParams`, Edit + Tools menus): re-opens the parsed
  viewer for the current document. Handles both PNG (looks up the `parameters`/`prompt`
  chunk) and foreign files (re-reads EXIF via `twpng_ReadJpegAIParams` /
  `twpng_ReadWebpAIParams`). Menu enabling lives in `WM_INITMENU` and explicitly
  re-enables the item when only a foreign file is loaded (the standard `cmdlist1` loop
  greys everything that requires `png != NULL`).
- **Read AI parameters from JPEG/WebP** (Tools menu, `ID_READAIFROMFILE`): one-shot file
  picker that pops the parsed viewer without loading the file as a document.
- **Strip AI Metadata** (`StripAIMetadata`, Edit menu): removes AI metadata chunks
  (`twpng_IsAIMetadataChunk`: parameters/workflow/prompt/NovelAI keys/`eXIf`...) after a
  confirm, for sharing without leaking prompts. PNG-only — JPEG/WebP EXIF stripping is
  not implemented.
- **Generator badge**: `twpng_DetectGenerator(Png*)` walks tEXt keywords for the PNG
  path; `twpng_DetectGeneratorFromText(const TCHAR*)` sniffs the raw EXIF text
  (`"class_type"` → ComfyUI, `"uc"`+`"prompt"` → NovelAI, `Steps:` → A1111) for the
  foreign path. The result is cached in `g_foreignGen` at load time and appended to the
  status-bar size line.

## Foreign image formats (JPEG / WebP)

The app is still PNG-first — no decoder, no chunk table, no editing for JPEG/WebP. But
opening one of these files from the menu/drag-drop/command line goes through a
metadata-only path so AI prompts can be inspected.

- **State**: three globals in `tweakpng.cpp` — `g_foreignFn` (path), `g_foreignKind`
  (1=JPEG, 2=WebP), `g_foreignGen` (cached badge string). When set, `png == NULL` but
  the title bar, status bar, View AI Parameters, and Close Document all treat the
  foreign file as "the current document."
- **Open path**: `OpenPngByName` sniffs the first bytes (`FF D8` for JPEG,
  `RIFF....WEBP` for WebP), routes foreign files through a branch that sets the globals,
  updates the title/status, runs the EXIF reader, and triggers auto-open if enabled.
- **Close path**: `ClosePngDocument` clears all three globals and also calls
  `twpng_CloseAIParamsView()` (the PNG path gets that for free from `Png::~Png`).
  `OpenPngByName` does the same close-stale-viewer dance when replacing one foreign
  file with another.
- **EXIF readers**: `twpng_ReadJpegAIParams` walks JPEG markers via `twpng_FindJpegExif`
  to locate the APP1 EXIF segment; `twpng_ReadWebpAIParams` walks RIFF chunks for an
  `EXIF` chunk. Both call `twpng_ExtractFromExifTiff` → TIFF IFD walker → ExifIFD
  (`0x8769`) → UserComment (`0x9286`) → `twpng_DecodeUserComment`.
- **Civitai quirks the readers handle**: (1) some writers (Civitai, A1111 exporters)
  emit a tiny APP1 segment whose declared length doesn't cover the actual UserComment
  payload — `twpng_FindJpegExif` deliberately reports the TIFF length as
  `len - tiffOff` (rest of file) rather than the segment's declared length, so the TIFF
  parser can still reach the data. (2) UTF-16 UserComment byte order can differ from
  the enclosing TIFF byte order, so `twpng_DecodeUserComment` auto-detects BE vs LE
  from the count of zero bytes at even vs odd positions instead of trusting the TIFF
  header.

## Conventions

- TCHAR / `_T()` throughout; build can be Unicode or non-Unicode (non-Unicode drops iTXt).
- Match the existing terse Win32 style. Keep comments to non-obvious "why" only.
