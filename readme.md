TweakPNG
========

A Windows application for viewing and editing the low-level chunk structure of PNG
image files.

This is a fork of [jsummers/tweakpng](https://github.com/jsummers/tweakpng) with two
goals:

**Modern Windows 10/11 UI** — a dark-mode title bar, menu bar, chunk list, dialogs,
message boxes, and color picker, plus PerMonitorV2 DPI awareness, rounded corners, and a
Mica backdrop.

**Quality-of-life features for AI-generated images** (Automatic1111 / Stable Diffusion):

- **Auto-open parameters** — opening an image that has a `parameters` tEXt chunk pops up
  a viewer automatically (toggle in Options → Preferences).
- **AI Generation Parameters viewer** (Edit → View AI Parameters) — splits the metadata
  into Prompt, Negative prompt, and Settings, each with a Copy button (plus Copy All).
- **Strip AI Metadata** (Edit → Strip AI Metadata) — removes prompt/seed/workflow/EXIF
  metadata so an image can be shared privately.
- **Generator badge** — the status bar identifies A1111, ComfyUI, or NovelAI images.

Building
--------

Open `proj/vs2022/tweakpng.sln` in Visual Studio 2022 and build the `Release`/`x64`
configuration (output: `Release64/tweakpng.exe`).

The default build links `libpng` and `zlib`, expected as sibling directories of the
repository root. To build without third-party libraries, comment out the feature flags
in `twpng-config.h`. See `tweakpng-src.txt` for the full build matrix, and `CLAUDE.md`
for repository structure, dark-mode implementation notes, and the AI-image features.

Documentation
-------------

For usage instructions, see `tweakpng.txt`.

Original project web site: [entropymine.com/jason/tweakpng](https://entropymine.com/jason/tweakpng/)

