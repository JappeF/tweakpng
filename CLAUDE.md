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
`Debug32`/`Release32`/`Debug64`/`Release64`. Also links `comctl32.lib` and `uxtheme.lib`, plus
(via `#pragma comment` in `tweakpng.cpp`) WIC, msimg32, shlwapi and Media Foundation
(`mfplat.lib`, `mfuuid.lib`, `mfreadwrite.lib`) and `winmm.lib` for video playback.

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

- **Theme selector** (Options > Theme: System default / Light / Dark, registry value
  `theme`, default Dark). `globals.theme_pref` is resolved into `globals.dark` by
  `twpng_ResolveTheme` (System reads `AppsUseLightTheme`). Everything dark is gated on
  `globals.dark`; in light mode `hUiBgBrush`/`hDarkEditBrush` are NULL, which also turns
  off the `WM_CTLCOLOR*` dark paths. `twpng_ApplyTheme` switches live: app mode, brushes,
  main window controls (`twpng_ThemeMainControls`) and every visible dialog / image
  viewer (re-running `twpng_InitDarkDialog`, whose child proc also undoes the dark
  subclasses and themes). `WM_SETTINGCHANGE` "ImmersiveColorSet" re-applies it when the
  theme is System and Windows flips.
- App-level mode: `SetPreferredAppMode` (2 = ForceDark, 3 = ForceLight) set in
  `twpng_SetAppMode` after `ReadSettings` and before any window exists, plus
  `AllowDarkModeForWindow`, `RefreshImmersiveColorPolicyState` and `FlushMenuThemes` via
  `uxtheme.dll` ordinals (133/135/104/136). Title bar/corners/backdrop via
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
  painted over, not removed, to keep single-line text centered). Its `WM_NCPAINT` hands the
  default a region without that 2-pixel edge, so the light edge never shows first (it blinked
  on every resize step).
- **Message boxes**: the OS `MessageBox` can't be fully darkened (it paints a light footer
  in `WM_PAINT`), so `twpng_MessageBox` is a custom dark dialog (`DLG_MSGBOX`) used by
  `mesg()` and every other call site. Supports MB_OK/OKCANCEL/YESNO/YESNOCANCEL + icons.
- **About dialog acrylic**: `twpng_ApplyAcrylic` sets backdrop type 3 (acrylic) and
  extends the DWM frame over the whole client area; `DlgProcAbout` returns a black brush
  with transparent bk mode for `WM_CTLCOLOR*`, since black is what lets the blur show
  through. Windows shows acrylic as a flat fill while the window is inactive. In the light
  theme dark GDI text would be invisible (alpha 0), so every child is subclassed with
  `GlassChildSubclassProc`: it paints the control onto white in a buffered-paint DIB via
  `WM_PRINTCLIENT` and converts white to transparency (premultiplied alpha). Needs
  `BufferedPaintInit` (called in WinMain).
- **System common dialogs** (the `ChooseColor` picker) are dark-themed via a scoped
  `WH_CBT` hook in `twpng_ChooseColorDark`, which themes the chrome on activation while
  leaving owner-drawn swatches/spectrum colorful.

The main window has a real menu (`MENUMAIN`); commands are dispatched in `WM_COMMAND` and
enabled/disabled in `WM_INITMENU`. There is no separate toolbar.

## Modeless editors

Two windows run modeless and **unowned** (so they minimize independently of the main
window and get their own taskbar buttons), one instance each:

- the tEXt/zTXt/iTXt editor (`DlgProcEdit_tEXt`, opened from `Chunk::edit()`), and
- the AI parameters viewer (`DlgProcAIParams` / `twpng_ShowAIParams`).

The main message loop routes their messages via `IsDialogMessage`
(`twpng_IsModelessEditorMessage` for the editor; a direct `g_hwndAIParams` check for the
viewer). Both are force-closed when the document is torn down — `Png::~Png` calls
`twpng_CloseModelessEditors()` and `twpng_CloseAIParamsView()` — and free their
heap context in `WM_NCDESTROY`. The tEXt editor edits live chunk data, so deleting its
chunk also closes it (`twpng_CloseEditorForChunk`); the AI viewer holds a copy, so it is
safe regardless. A `Png` with `m_scratch` set (a helper copy used to read a PNG that
isn't the open document) skips that close in its destructor.

## A1111 / AI image features

For PNGs, all keyed off text chunks (mainly tEXt keyword `parameters`); for JPEG/WebP,
keyed off the EXIF UserComment tag; for MP4/MOV, keyed off QuickTime metadata (see
"Foreign image formats" below).

- **Auto-open** (`twpng_AutoOpenParamsText`, end of `OpenPngByName`): if the Preferences
  option `open_params_on_load` is set (default on), opening an image with a `parameters`
  chunk pops up the parsed viewer. The option persists in the registry (`open_params`).
  The foreign (JPEG/WebP/MP4/MOV) path triggers the same auto-open from its branch in
  `OpenPngByName`.
- **Parsed viewer** (`DLG_AIPARAMS`, `twpng_ShowAIParams`): splits the A1111 string into
  Prompt / Negative prompt / Settings with copy buttons. `twpng_BuildAICtx` parses the
  text and appends `extra` (container info such as the MP4 video size) to the settings.
  Showing again while the viewer is open updates it in place (new ctx swapped in). The
  ctx remembers the source `path` and whether it is the open document (`isDoc`).
  - **Prev / Next** (`twpng_AIParamsNavigate`, also Left/Right, Alt+Left/Right and
    Ctrl+PageUp/Down via `twpng_AIParamsKey` in the message loop; Shift+arrows stay with
    the edit boxes): steps through the supported files in
    the same folder; the title shows the position. The listing (`twpng_ListDir`, natural
    sort via `StrCmpLogicalW`) is cached in `g_sib` and built on a background thread
    (`WM_APP_SIBLINGS` back to the main window), because folders can hold 20k+ files on
    slow network drives. `twpng_GetSiblings` re-lists when the folder changes (its
    last-write time), the list is older than 5 minutes, the file is missing from it, or
    F5 is pressed; the old list stays usable meanwhile. For the open document it opens the next file as the document, with
    `g_keepAIView` set so the document change doesn't close the viewer; otherwise it just
    reads the next file (`twpng_ReadAnyAIParams`).
  - **Thumbnail**: `twpng_RequestThumb` asks the shell (`IShellItemImageFactory`,
    `SIIGBF_THUMBNAILONLY`) for Explorer's thumbnail on a background thread and posts it
    back (`WM_APP_THUMB`); a generation counter drops results for files no longer shown.
    It is owner-drawn (`IDC_AIP_THUMB`, `SS_NOTIFY`) right of the settings box, and
    subclassed (`ThumbSubclassProc` -> `twpng_ThumbMouse`): right click = next, left click =
    previous after a `TWPNG_CLICKTIMER` of the double-click time (so a double-click can still
    open the full-screen view without browsing first).
  - **Splitters**: the gaps between Prompt / Negative prompt / Settings and between
    Settings and the thumbnail are draggable (`g_aipSplit`, hit-tested in the dialog's
    own `WM_SETCURSOR` / `WM_LBUTTONDOWN`, since the gaps and the static labels belong to
    the dialog). Positions live in `globals.aip_split_prompt/neg/thumb` (per mille,
    registry `aip_split_*`, thumb 0 = from the image aspect); `WM_LBUTTONDBLCLK` on a
    splitter resets it. The dialog's own `WM_PAINT` draws five-dot grips in the gaps
    (`twpng_AIPDrawGrips`, brighter on hover via `TrackMouseEvent` / `WM_MOUSELEAVE`).
    Against flicker while dragging/resizing: `twpng_LayoutAIParams` moves everything in one
    `DeferWindowPos` batch (`twpng_LayMove`) and then invalidates only the dialog (which has
    `WS_CLIPCHILDREN`, swallows `WM_ERASEBKGND` and fills the background in `WM_PAINT`) and
    the thumbnail (no erase, drawn through a buffer). Don't go back to
    `RedrawWindow(RDW_ERASE|RDW_ALLCHILDREN)` there: it blinks every edit box on each step.
  - **Full screen** (double-click the thumbnail or F11; `twpng_FullscreenOpen`,
    `FullWndProc`, class `TWPNGFULLVIEW`): a popup covering the viewer's monitor.
    `twpng_DecodeFit` decodes on a worker thread (WIC, scaled to the screen, premultiplied
    BGRA; videos and anything WIC can't read fall back to a large shell thumbnail) and
    posts `WM_APP_FULLIMG`; the small thumbnail is drawn meanwhile. Left/Right, PageUp/Down,
    Space (images), left/right click and the mouse wheel browse (through
    `twpng_AIParamsNavigate`, so the viewer follows), Esc / F11 / middle click close. The
    class has no `CS_DBLCLKS`, so fast clicks stay clicks. Its messages (and its children's)
    bypass the main window's accelerators in the message loop (Delete would otherwise
    delete a chunk), and `twpng_ShowAIParams` doesn't pull the viewer to the front while it
    is open.
  - **Video playback** (`IDC_AIP_PLAY` / `SLOWER` / `SPEED` / `FASTER`, shown only for
    .mp4/.mov/.m4v, `twpng_IsVideoPath`): a Media Foundation media engine
    (`IMFMediaEngine`, rendering mode via `MF_MEDIA_ENGINE_PLAYBACK_HWND`) renders into a
    child window of class `TWPNGVIDEO` (`g_vid.host`), placed over the thumbnail's image
    area or fitted to the whole full-screen view (`twpng_VideoPlace`). Since the video
    covers the full-screen window's own status line, that line (`twpng_FullStatusText`)
    then shows in an owned, click-through popup (`twpng_FullOSD`, class `TWPNGOSD`,
    `WS_EX_LAYERED|WS_EX_TRANSPARENT`) drawn with `UpdateLayeredWindow` per-pixel alpha:
    grayscale-antialiased text coverage plus a spread copy as a dark edge, the rest fully
    transparent. Updated from `twpng_VideoUI`. The dialog and the full-screen window have `WS_CLIPCHILDREN` (and
    the thumbnail `WS_CLIPSIBLINGS`) so GDI painting doesn't overwrite the video. Engine
    events arrive on an MF thread and are posted to the host as `WM_APP_VIDEO`; the host is
    shown at the first frame and forwards clicks (posted, since browsing destroys it).
    There is one engine at a time: moving between viewer and full screen recreates it at
    the same position (`twpng_VideoMoveTo`, seek applied at `LOADEDMETADATA`), and
    `twpng_VideoFileChanged` (from `twpng_ShowAIParams`) stops it or, if it was playing,
    starts the next file. Speed is `g_vidRate` percent (25..400, steps of 25) through
    `SetPlaybackRate`. Space toggles unless the focus is an Edit; -/+ always.
  - **Video sound**: the engine time-stretches (pitch kept) with no way to turn that off,
    so it is muted and the sound is played by us with the pitch following the speed.
    `twpng_PcmThread` decodes the first audio track with `IMFSourceReader` to 16-bit PCM
    (max 10 minutes, cached in `g_pcm` for the last file, posted back as `WM_APP_PCM`; a
    decode already running for the same file is not restarted, and `twpng_VideoShutdown`
    waits for it before `MFShutdown`);
    `twpng_AudioTryOpen` (once PCM and metadata are there) opens `waveOut` at the file's
    rate with a pump thread (`CALLBACK_EVENT`, 5 x 25 ms buffers) that reads the PCM at
    `step` = speed with linear interpolation, wrapping at the video's duration. The video
    is the clock: `twpng_AudioSync` (150 ms host timer and engine events) follows
    pause/play/speed and restarts the sound at the engine position on more than 0.1 s
    drift, except within 0.5 s of the loop point, where the engine stalls briefly. If
    decoding fails the engine's own sound is unmuted.
  - **Show in Explorer** (`IDC_AIP_EXPLORER`): `SHOpenFolderAndSelectItems`; an already
    open Explorer window on that folder is reused.
  - **Save JSON** writes `ctx->full` as UTF-8 when it is JSON (the ComfyUI graph), so it
    can be loaded back into ComfyUI. Parser `twpng_ParseA1111` splits on the
  `Negative prompt:` and `Steps:` line markers; `twpng_SettingsToLines` reformats the
  settings one-per-line while respecting double-quoted values (e.g. `Lora hashes: "..."`).
- **ComfyUI parser** (`twpng_ParseComfyJson`): ComfyUI stores its graph as JSON in
  a `prompt` tEXt chunk, EXIF UserComment or MP4 `prompt` key; this walks the node graph
  with brace-aware substring scans (not a full JSON parser) and synthesizes an
  A1111-style string the parsed viewer can consume. Helpers: `cf_find_class_inputs`
  (first node of a class), `cf_node_inputs_by_id`. For `SamplerCustom(Advanced)` the
  settings live in the nodes behind its `noise`/`sampler`/`sigmas`/`guider` refs, and the
  prompt is reached through the guider (`positive`, `conditioning` or `cond1`). Prompt
  resolution follows refs until a node has a string `text`/`prompt` input, so video
  nodes (Wan/MiniMax `*ToVideo`) work too. Video graphs also add `Frames:`/`FPS:`.
  The first `*ToVideo` node on the positive path gives `Mode:` (`cf_video_mode`, from
  the class name: ReferenceToVideo / ImageToVideo / FirstLastFrame / TextToVideo) and
  its input images (`cf_collect_images`: `ref_image*` keys as a quoted `References:`
  list, `first_frame`/`start_image` and `last_frame`/`end_image` as `First frame:` /
  `Last frame:`), resolved back to the `LoadImage` file name by `cf_resolve_image`.
  Text inputs go through `cf_get_text`, which follows a wired input (e.g. `prompt` fed
  by a `PrimitiveStringMultiline`) to the string node's `value`/`text`/`string`.
  LoRAs come from every node whose class contains `Lora` (`cf_collect_loras`:
  `lora_name` + `strength_model`, or rgthree Power Lora Loader `lora_N` objects with
  `on`/`lora`/`strength`), shown as `LoRA:` or a quoted `LoRAs:` list.
- **View AI Parameters** (`twpng_ViewAIParams`, Edit + Tools menus): re-opens the parsed
  viewer for the current document. Handles both PNG (looks up the `parameters`/`prompt`
  chunk) and foreign files (`twpng_GetDocAIText`). Menu enabling lives in `WM_INITMENU`
  and explicitly re-enables the item (and Strip AI Metadata) when only a foreign file is
  loaded (the standard `cmdlist1` loop greys everything that requires `png != NULL`).
  Commands that must work without a PNG are dispatched outside the `if(png) switch(id)`
  block in `WM_COMMAND`.
- **Read AI parameters from File** (Tools menu, `ID_READAIFROMFILE`): one-shot file
  picker that pops the parsed viewer without loading the file as a document. Any
  supported type, PNG included (`twpng_ReadAnyAIParams` reads PNGs via a scratch `Png`).
- **Compare AI Parameters** (Tools menu, `ID_COMPAREAI`, `DLG_AICOMPARE`): pick two
  files (or one, compared with the open document). `twpng_BuildCompareReport` lists
  changed / one-sided / identical settings keys and a word diff of the prompts
  (`twpng_DiffWords`: LCS over whitespace tokens, `[-removed-]` / `{+added+}`, capped at
  3000 words per side).
- **Strip AI Metadata** (`StripAIMetadata`, Edit menu): for PNG removes AI metadata
  chunks (`twpng_IsAIMetadataChunk`: parameters/workflow/prompt/NovelAI keys/`eXIf`...)
  after a confirm. For a loaded JPEG/WebP/MP4/MOV (`twpng_StripForeignMetadata`) it
  saves a copy (default `<name>_clean.<ext>`; picking the same name replaces the file):
  JPEG drops APP1 (EXIF/XMP), APP13 and COM segments and skips junk left by short APP1
  lengths; WebP drops `EXIF`/`XMP ` chunks and fixes the VP8X flags and RIFF size; MP4
  renames `moov/udta` and `moov/meta` to `free` and zeroes their contents in place, so
  no offsets move and the media data is untouched. The save dialogs get a full default
  path (the dialog ignores `lpstrInitialDir` in favour of its remembered folder).
- **Generator badge**: `twpng_DetectGenerator(Png*)` walks tEXt keywords for the PNG
  path; `twpng_DetectGeneratorFromText(const TCHAR*)` sniffs the raw EXIF text
  (`"class_type"` → ComfyUI, `"uc"`+`"prompt"` → NovelAI, `Steps:` → A1111) for the
  foreign path. The result is cached in `g_foreignGen` at load time and appended to the
  status-bar size line.

## Foreign image formats (JPEG / WebP / MP4 / MOV)

The app is still PNG-first — no decoder, no chunk table, no editing for JPEG/WebP. But
opening one of these files from the menu/drag-drop/command line goes through a
metadata-only path so AI prompts can be inspected.

- **State**: three globals in `tweakpng.cpp` — `g_foreignFn` (path), `g_foreignKind`
  (1=JPEG, 2=WebP, 3=MP4, 4=MOV), `g_foreignGen` (cached badge string). When set, `png == NULL` but
  the title bar, status bar, View AI Parameters, and Close Document all treat the
  foreign file as "the current document."
- **Open path**: `OpenPngByName` sniffs the first bytes via `twpng_SniffForeignKind`
  (`FF D8` for JPEG, `RIFF....WEBP` for WebP, `ftyp` at offset 4 for MP4, brand `qt  `
  or a bare `moov`/`wide`/`mdat`/`free` box for MOV), routes foreign files through a
  branch that sets the globals, updates the title/status, runs the reader
  (`twpng_ReadForeignAIParams` dispatches by kind), and triggers auto-open if enabled.
- **Close path**: `ClosePngDocument` clears all three globals and also calls
  `twpng_CloseAIParamsView()` (the PNG path gets that for free from `Png::~Png`); that
  function is a no-op while `g_keepAIView` is set (folder browsing).
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
- **MP4/MOV reader** (`twpng_ReadMp4AIParams`): seeks over top-level boxes (64-bit
  sizes, so multi-GB videos are fine) and reads only `moov` (it can sit before or after
  `mdat`). Metadata is collected from `moov/udta/meta` and `moov/meta` (`keys` + `ilst`;
  item types are 1-based key indexes when a `keys` table exists, else fourccs) and from
  the classic QuickTime `udta` comment box (0xA9 `cmt`). Preference order: `prompt` (ComfyUI
  native SaveVideo, mdta key), `parameters`, `comment` (VideoHelperSuite), `description`.
  VHS wraps the graph as `{"prompt": "<json string>", "workflow": ...}`; the reader
  unwraps the `prompt` value (string or object) when it looks like a ComfyUI graph.
  `twpng_ReadMp4VideoInfo` adds the real `Video size`, `Frames` (sample count) and
  `Duration` of the first video track (`tkhd`, `mdhd`, `stsz`) to the viewer settings,
  since graphs often compute these (ResolutionSelector, math nodes) instead of storing
  them.

## Folder overview (search and CSV)

Tools > Folder Overview is a submenu built in `WM_INITMENU` (`twpng_FolderMenu`): current
folder (`ID_AIFOLDER`), Choose Folder (`ID_AIFOLDER_PICK`), up to 20 indexed folders
(`ID_AIFOLDER_IDX` + n, paths kept in `g_folderMenuDirs`, dispatched before the main
`WM_COMMAND` switch) and Manage Folder Indexes. Inside the dialog, Change... pops a similar
menu under the button (`twpng_FolderChangeMenu`), and `WM_CONTEXTMENU` on the list
(`twpng_FolderContextMenu`) offers Open in Viewer / Show in Explorer (all selected files in
one window, `twpng_ShowFilesInExplorer`) / Copy Full Path / Copy Prompt for the selected rows.
Ctrl+F in the viewer opens the modeless,
unowned `DLG_AIFOLDER` (`DlgProcAIFolder`, `g_hwndAIFolder`, routed in the message loop
like the viewer). It lists every supported file of a folder in a virtual list view
(`LVS_OWNERDATA`, text from `LVN_GETDISPINFO`; `twpng_FindSetting` pulls single values
out of a settings line), filtered by the search box (every word must appear in the file
name, prompts or settings; 250 ms debounce timer), double-click opens the file in the
viewer (`twpng_OpenInViewer`), and Export CSV writes the shown rows (UTF-8 with BOM,
fixed columns for the common settings plus "Other settings").

- **Scan** (`twpng_ScanThread`, one `struct twpng_scan` per run, ref-counted between the
  dialog and the worker): lists the folder with sizes and times, allocates all records
  up front (so the UI can read `recs[0..done)` without locks while the worker fills the
  rest), and posts `WM_APP_SCAN` progress about every 100 ms. Stop / closing the dialog
  only sets `cancel`; the worker frees the scan when it ends.
- **Metadata-only reads** (`twpng_ScanFileAI`, no UI, worker-safe): PNG text chunks with
  IDAT skipped by seeking (`twpng_ScanPngAIText`, iTXt/zTXt via `twpng_Inflate`), the
  first 2 MB of a JPEG, WebP chunk headers up to `EXIF`, and the MP4 `moov` readers.
  Results are parsed with `twpng_BuildAICtx` like the viewer does.
- **Sorting**: `LVN_COLUMNCLICK` -> `twpng_FolderSortClick`; `twpng_FolderSortRows` sorts
  `f->rows` with precomputed keys (`struct twpng_skey`, columns `AFC_*`; ties in natural
  name order); File ascending is the records' own order. Done again whenever rows are added.
  The header gets `HDF_SORTUP/DOWN` (the dark header custom draw in `MainListSubclassProc`
  paints the arrow itself). Remembered as `globals.aif_sort` (registry `aif_sort`: column+1,
  negative = descending). Selection, focus and top row are kept by file name
  (`twpng_FolderSavePos` / `twpng_FolderRestorePos`).
- **Watching** (`twpng_FolderWatch`, `twpng_WatchThread`): `FindFirstChangeNotification`
  plus a 10 s check of the folder's time (network/cloud drives) posts
  `WM_APP_FOLDERCHANGED` (generation-checked); after 1.5 s of quiet `twpng_FolderQuietScan`
  starts a second scan (`f->pending`, reading only new/changed files thanks to the index)
  and `twpng_FolderSwapIn` replaces the rows when it is done. A change during a scan sets
  `f->dirty` and rescans afterwards; after the user pressed Stop nothing rescans until
  Rescan. The watch thread frees itself after its stop event. The scan skips rewriting the
  index when nothing changed. While a first scan runs with a sort active, rows are re-sorted
  at most once a second (and once at the end), keeping the selection by name.
- **Index** (`twpng_LoadIndex` / `twpng_SaveIndex`): `%LOCALAPPDATA%\TweakPNG\index\<FNV-1a
  of the folder path>.idx`, a binary file (magic `TWAI`, version 1) with per file name,
  size, last-write time and the parsed prompt / negative / settings in UTF-8. A file is
  only read again when its size or time changed. A stopped scan still saves what it has,
  plus the old records of files it didn't reach.
- **Options > Folder Indexes** (`ID_AIINDEX`, `DLG_AIINDEX`, `DlgProcIndexes`): lists the
  index files with the folder and file count from their headers, size and date, and
  deletes the selected ones or all after a confirm.
- Measured: 22,000 local files take about 3.4 s the first time and 0.2 s from the index;
  on RaiDrive (cloud WebDAV) a first read costs about 50 ms per video.

## Explorer menu and `/aiparams`

`tweakpng.exe /aiparams "<file>"` starts with the main window hidden and shows only the
AI parameters viewer (as the open document, so Prev/Next work); closing the viewer
closes the app (`WM_NCDESTROY` posts `WM_CLOSE` when `globals.aiparams_only`). The
Preferences checkbox "Add View AI parameters to the Explorer menu" registers that
command per user under
`HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\TweakPNG.AIParams` for
.png/.jpg/.jpeg/.webp/.mp4/.mov/.m4v (`set_ai_menu`), independent of the file type's
ProgID (unlike the older "Add TweakPNG to Explorer context menu" option).

## Conventions

- TCHAR / `_T()` throughout; build can be Unicode or non-Unicode (non-Unicode drops iTXt).
- Match the existing terse Win32 style. Keep comments to non-obvious "why" only.
