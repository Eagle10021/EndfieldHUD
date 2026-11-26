#!/usr/bin/env python3
"""Extracts the graphics-API-independent parts of windows/native/renderer.cpp.

Draw-object validation, the GPU uniform layout and the texture conversions stay identical to
upstream because they are copied from it at build time. Usage: extract_renderer_logic.py IN OUT
"""
import sys

if sys.argv[1] == "--unguard":
    text = open(sys.argv[2], encoding="utf-8").read()
    assert "#ifdef _WIN32\n" in text, "expected a Windows guard in " + sys.argv[2]
    open(sys.argv[3], "w", encoding="utf-8").write("// Generated from " + sys.argv[2].rsplit("/windows/", 1)[-1] + ". Do not edit.\n" + text.replace("#ifdef _WIN32\n", "#if 1\n"))
    raise SystemExit(0)

if sys.argv[1] == "--source-scene":
    text = open(sys.argv[2], encoding="utf-8").read()
    parts = text.split("#ifdef _WIN32\n    ")
    assert len(parts) == 3 and text.count("requires native Windows graphics") == 2, "source_scene.cpp changed"
    open(sys.argv[3], "w", encoding="utf-8").write("// Generated from windows/native/source_scene.cpp. Do not edit.\n" + "#if 1\n    ".join(parts))
    raise SystemExit(0)

if sys.argv[1] == "--raster-relief":
    lines = open(sys.argv[2], encoding="utf-8").read().split("\n")
    first = next(i for i, l in enumerate(lines) if l.startswith("namespace {thread_local std::string_view rasterAttribution;}"))
    end = next(i for i, l in enumerate(lines) if l.startswith("LayerRasterStats LayerRasterizer::stats()"))
    open(sys.argv[3], "w", encoding="utf-8").write("// Generated from windows/native/layer_raster.cpp. Do not edit.\n" + "\n".join(lines[first:end]) + "\n")
    raise SystemExit(0)

source, output = sys.argv[1:3]
lines = open(source, encoding="utf-8").read().split("\n")

def index(prefix, start=0):
    for i in range(start, len(lines)):
        if lines[i].startswith(prefix):
            return i
    raise SystemExit(f"renderer.cpp changed: no line starting with {prefix!r}")

def between(first, end):
    a = index(first)
    return lines[a:index(end, a + 1)]

parts = []
parts += ["namespace endfield::native {", "namespace {"]
parts += between("void require(", "double qpcSeconds(")
parts += between("const std::array<double, 256> &linearSRGBBytes()", "ComPtr<ID3DBlob> compile(")
parts += ["} // namespace", "namespace detail {"]
detail = index("namespace detail {")
# Everything in the detail namespace except the final helper, which returns a DXGI format.
end = index("// Every texture color space at four bytes per texel", detail)
parts += lines[detail + 1:end]
parts += ["} // namespace detail"]
parts += between("void validatePlaneShutter(", "RendererError::RendererError(")
a = index("RendererError::RendererError(")
parts += lines[a:a + 2]
parts += ["} // namespace endfield::native", ""]

head = ["// Generated from windows/native/renderer.cpp. Do not edit.", '#include "native/renderer.hpp"'] + \
    [f"#include <{h}>" for h in ("algorithm", "array", "cmath", "cstdint", "limits", "span", "stdexcept", "string", "variant")]
open(output, "w", encoding="utf-8").write("\n".join(head + parts))
