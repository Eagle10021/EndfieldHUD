# Native Linux port

An unofficial Linux port of [EndfieldHUD](https://github.com/DDDuoDuo/EndfieldHUD) by [DDDuoDuo](https://github.com/DDDuoDuo). The app, its design and
its code are the original author's work; see [LICENSE](../LICENSE) (MIT) and [CREDITS.md](../CREDITS.md),
which also lists the game assets the MIT license does not cover. Official macOS and Windows releases
are in the [original repository](https://github.com/DDDuoDuo/EndfieldHUD/releases).

Built from the original sources in `../windows`; no upstream source file is modified.

## Build and run

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build
    tools/fetch_resources.sh      # official release assets, checked by SHA-256 (about 45 MB)
    build/endfieldhud             # Ctrl+Tab or the tray icon opens the HUD

`tools/install.sh [--autostart]` adds a menu entry (and starts it at login); `--remove` undoes it.
The program runs from the build directory.

Needs: g++, CMake, Ninja, Python, curl, bsdtar, glslang, ICU, zlib, SQLite, libepoxy, EGL with OpenGL
4.6, Cairo, Pango, fontconfig, HarfBuzz, gdk-pixbuf, vkd3d (libvkd3d-shader), and for the overlay
Qt 6, layer-shell-qt, KGlobalAccel and KWindowSystem (Arch package names: `vkd3d`, `layer-shell-qt`,
`kglobalaccel`, `kwindowsystem`). `-DBUILD_APP=OFF` leaves out the overlay and its KDE libraries.

Control and testing:

    build/endfieldhud toggle|open|close|quit    # the running instance, over the session bus
    build/endfieldhud --preview 3               # shows once without taking input
    build/hud_preview <resources> platform/shaders frame.png [width height seconds]

Environment: `ENDFIELDHUD_RESOURCES` (resources directory), `ENDFIELDHUD_SCREEN` (output name, instead
of the active screen), `ENDFIELDHUD_FPS` (frame limit, default 60; 0 follows the screen), `ENDFIELDHUD_NO_BLUR`,
`ENDFIELDHUD_TIMING` (frame statistics after closing).

## State (2026-11-25)

Works: the main screen as on Windows (original material shaders, opening, closing and hover motion,
navigation cards with icons and captions, header, clock, footer), full screen on the active monitor
with the desktop dimmed and blurred; mouse, wheel, Esc, Ctrl+Tab, click outside to close; the power
button quits; a Terminal tile (Linux addition) opens kitty.

Not yet: every module inside the HUD (cards other than Terminal do nothing), settings and saved
preferences (defaults apply, English only), the profile card's account name, saved app shortcuts,
the charge and facility overlays, Projection, and notifications.

How the pieces map to the Windows build:

| Windows | Linux |
| --- | --- |
| `native/renderer.cpp` (Direct3D 11) | `platform/renderer_gl.cpp` (OpenGL). Not yet: content filter, media and worker textures, device suspension |
| `native/source_graphics.cpp` | `platform/source_graphics_gl.cpp`; the release's Direct3D shader bytecode is translated to SPIR-V by libvkd3d-shader |
| `native/layer_raster.cpp` (Direct2D, DirectWrite) | `platform/layer_raster_linux.cpp` (Cairo, Pango). Not yet: editor documents (plain and rich text viewports, hit testing); `textLayout()` returns none |
| `app/application.cpp` | `platform/hud_shell.cpp` (the main screen only) and `app/main.cpp` |
| `native/desktop_backdrop.cpp` (Windows composition) | `platform/presenter_gl.cpp` for tint and vignette, KWin for blur (its strength follows the desktop's blur setting) |

Upstream sources are built unmodified. Where portable code sits behind a Windows guard or inside a
Windows-only file, `tools/extract_renderer_logic.py` generates the Linux copy at build time. Files
that need Windows headers are listed in `windows-only-sources.txt`.

Tests: `renderer` (output pixels), and the upstream `layer_mask`, `layer_source_text`,
`layer_image_source` and `layer_raster` programs; the last omits its text metadata budget block, which
needs retained editor layouts.

Frame rate: limited to 60 per second. Without the limit, a Ryzen 7 5800H laptop (integrated
graphics) reached about 160 on the 165 Hz built-in panel and 95 to 125 on a 180 Hz HDMI monitor wired
to the NVIDIA graphics.

## Plan

1. Done: portable code, renderer, text and layers, original material pass, overlay with Ctrl+Tab.
2. Next: the module area, starting with Storage, Activity Monitor and Volume.
3. Settings and saved preferences, app shortcuts, Now Playing, the remaining modules.
