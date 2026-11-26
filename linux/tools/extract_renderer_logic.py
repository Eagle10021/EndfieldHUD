#!/usr/bin/env python3
"""Extracts the graphics-API-independent parts of windows/native/renderer.cpp.

Draw-object validation, the GPU uniform layout and the texture conversions stay identical to
upstream because they are copied from it at build time. Usage: extract_renderer_logic.py IN OUT
"""
import sys

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
