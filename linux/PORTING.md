# Native Linux port

Built from the original sources in `../windows`; the upstream files are not modified.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cmake --build build --target missing-symbols   # expected to fail while symbols are missing

Needs: g++, CMake, Ninja, Python, glslang, ICU, zlib, SQLite, libepoxy, EGL with OpenGL 4.5.

## State (2026-11-25)

- `libhud_upstream.a` compiles: all of `core`, all of `modules`, and `native` except the files in
  `windows-only-sources.txt`, plus `app/utility_executor.cpp`.
- `platform/renderer_gl.cpp` implements the renderer on OpenGL 4.5 for an offscreen target: meshes,
  textures (including in-place painting and parking), native groups, draw lists and composition. Draw
  validation and texture conversion come from the Direct3D renderer (`tools/extract_renderer_logic.py`).
  `ctest --test-dir build` checks output pixels. Not yet: on-screen presentation, content filter,
  media and worker textures, the original material pass (`source_graphics.cpp`), device suspension,
  and per-frame performance work (uniforms are uploaded on every draw call).
- The link check reports 15 symbols for the platform layer to provide:
  - `native::LayerRasterizer` and `LayerPlainTextAnalysis` (Direct2D / DirectWrite text and layers)
- Not covered by the link check: `app/` (window, overlay, input, system integrations), image loading in
  the Windows-only files, and wiring everything into a program.

## Plan

1. Done: portable code compiles; draw validation and the scene shader are ported.
2. Renderer: offscreen part done. Next: text and layers (Pango/Cairo or HarfBuzz) and images.
3. Layer-shell overlay on Plasma, input, Ctrl+Tab; show the dial.
4. Media, volume, system statistics, Kitty, settings, start at login.
