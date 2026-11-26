// Renders the dial offscreen from the resources of an official release and writes a PNG.
//   hud_preview <resources directory> <shader directory> <output.png> [width height seconds]
#include "hud_shell.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <chrono>
#include <iostream>

namespace gpu = endfield::native;
namespace core = endfield::core;

int main(int argc, char** argv) try {
    if (argc != 4 && argc != 7) { std::cerr << "usage: hud_preview <resources> <shaders> <output.png> [width height seconds]\n"; return 2; }
    const std::filesystem::path resources = std::filesystem::absolute(argv[1]);
    const unsigned width = argc == 7 ? unsigned(std::stoul(argv[4])) : 1920, height = argc == 7 ? unsigned(std::stoul(argv[5])) : 1080;
    const double seconds = argc == 7 ? std::stod(argv[6]) : 3;
    const auto clock = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const auto start = clock();
    gpu::Renderer renderer;
    gpu::RendererOptions options; options.target = gpu::RenderTarget::offscreenForTests; options.shaderPath = argv[2];
    renderer.initialize(nullptr, width, height, options);
    // Declared after the renderer: the shell releases its resources from it when destroyed.
    gpu::LayerRasterizer rasterizer;
    endfield::linux_port::HudShell shell(resources, rasterizer, core::Language::english);
    std::cout << "loaded in " << clock() - start << " ms\n";

    shell.resize({width, height, 1}, 0);
    shell.open(0, 0x5eed);
    double total = 0; unsigned frames = 0;
    for (double time = 0; time <= seconds + 1e-9; time += 1. / 60) {
        const auto begin = clock();
        if (!shell.frame(renderer, time)) continue;
        renderer.draw(false);
        if (frames++) total += clock() - begin;
        else std::cout << "first frame " << clock() - begin << " ms\n";
    }
    const auto frame = renderer.readback();
    const auto stats = renderer.sourceGraphicsStats();
    std::cout << frames << " frames, " << (frames > 1 ? total / (frames - 1) : 0) << " ms each, " << stats.draws << " passes, "
              << stats.pipelines << " programs, " << stats.textures << " textures\n";
    // The frame is premultiplied, as the desktop compositor expects: shown here over a flat backdrop.
    constexpr unsigned backdrop[3]{38, 42, 48};
    std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4);
    std::size_t clear = 0, partial = 0, over = 0;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        const unsigned alpha = frame.pixels[i + 3];
        clear += alpha == 0; partial += alpha > 0 && alpha < 255;
        over += frame.pixels[i] > alpha || frame.pixels[i + 1] > alpha || frame.pixels[i + 2] > alpha;
        for (unsigned c = 0; c < 3; ++c) rgba[i + c] = std::uint8_t(std::min(255u, frame.pixels[i + 2 - c] + (backdrop[c] * (255 - alpha) + 127) / 255));
        rgba[i + 3] = 255;
    }
    std::cout << "pixels: " << clear << " clear, " << partial << " translucent, " << over << " with colour above alpha\n";
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8, int(width), int(height), int(width) * 4, nullptr, nullptr);
    const bool saved = gdk_pixbuf_save(pixbuf, argv[3], "png", nullptr, nullptr);
    g_object_unref(pixbuf);
    if (!saved) throw std::runtime_error("Cannot write the image");
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
