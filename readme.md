TweakPNG
========

A Windows application for viewing and editing the low-level chunk structure of PNG
image files.

This is a fork of [jsummers/tweakpng](https://github.com/jsummers/tweakpng) that
modernizes the UI for Windows 10/11: a dark-mode title bar, menu bar, chunk list, and
dialogs, plus PerMonitorV2 DPI awareness, rounded corners, and a Mica backdrop.

Building
--------

Open `proj/vs2022/tweakpng.sln` in Visual Studio 2022 and build the `Release`/`x64`
configuration (output: `Release64/tweakpng.exe`).

The default build links `libpng` and `zlib`, expected as sibling directories of the
repository root. To build without third-party libraries, comment out the feature flags
in `twpng-config.h`. See `tweakpng-src.txt` for the full build matrix, and `CLAUDE.md`
for repository structure and dark-mode implementation notes.

Documentation
-------------

For usage instructions, see `tweakpng.txt`.

Original project web site: [entropymine.com/jason/tweakpng](https://entropymine.com/jason/tweakpng/)

