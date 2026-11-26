# Native Linux port

Built from the original sources in `../windows`; the upstream files are not modified.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cmake --build build --target missing-symbols   # expected to fail while symbols are missing

Needs: g++, CMake, Ninja, ICU, zlib, SQLite.

## State (2026-11-25)

- `libhud_upstream.a` compiles: all of `core`, all of `modules`, and `native` except the files in
  `windows-only-sources.txt`, plus `app/utility_executor.cpp`.
- The link check reports 34 symbols for the platform layer to provide:
  - `native::Renderer` and its draw/plane validation (D3D11 in `native/renderer.cpp`, shader `hud.hlsl`)
  - `native::LayerRasterizer` and `LayerPlainTextAnalysis` (Direct2D / DirectWrite text and layers)
- Not covered by the link check: `app/` (window, overlay, input, system integrations), image loading in
  the Windows-only files, and wiring everything into a program.

## Plan

1. Done: portable code compiles.
2. Implement the renderer (OpenGL or Vulkan), text (Pango/Cairo or HarfBuzz) and images.
3. Layer-shell overlay on Plasma, input, Ctrl+Tab; show the dial.
4. Media, volume, system statistics, Kitty, settings, start at login.
