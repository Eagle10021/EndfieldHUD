// Draws known content with the OpenGL renderer and checks the output pixels against the
// values the Direct3D shader math produces.
#include "native/renderer.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace endfield;
using namespace endfield::native;

static int failures = 0, checks = 0;
static double encode(double v) { return v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055; }
static double decode(double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); }

static void expect(const Readback& image, int x, int y, double r, double g, double b, double a, const std::string& what) {
    const auto* p = &image.pixels[std::size_t(y) * image.rowBytes + x * 4]; // BGRA
    const double got[4]{double(p[2]), double(p[1]), double(p[0]), double(p[3])}, want[4]{r, g, b, a};
    ++checks;
    for (int i = 0; i < 4; ++i)
        if (std::abs(got[i] - want[i]) > 1.5) {
            std::printf("FAIL %s at %d,%d: got %.0f %.0f %.0f %.0f, want %.1f %.1f %.1f %.1f\n", what.c_str(), x, y, got[0], got[1], got[2], got[3], r, g, b, a);
            ++failures; return;
        }
}
static void check(bool ok, const std::string& what) { ++checks; if (!ok) { std::printf("FAIL %s\n", what.c_str()); ++failures; } }

static std::vector<std::uint8_t> solid(int w, int h, int r, int g, int b, int a) {
    std::vector<std::uint8_t> v; for (int i = 0; i < w * h; ++i) { v.push_back(r); v.push_back(g); v.push_back(b); v.push_back(a); } return v;
}

int main() try {
    Renderer renderer;
    RendererOptions options; options.target = RenderTarget::offscreenForTests; options.shaderPath = SHADER_DIR;
    renderer.initialize(nullptr, 64, 64, options);
    core::Matrix4 camera; camera.values = {2. / 64, 0, 0, 0, 0, -2. / 64, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1}; // pixels, top-left origin
    renderer.setCamera(camera);
    const std::vector<Vertex> corners{{{0, 0, 0}, {0, 0}}, {{16, 0, 0}, {1, 0}}, {{16, 16, 0}, {1, 1}}, {{0, 16, 0}, {0, 1}}};
    const std::vector<std::uint32_t> order{0, 1, 2, 0, 2, 3};
    check(renderer.setMesh("quad", 1, {corners, order}), "mesh upload");
    check(!renderer.setMesh("quad", 1, {corners, order}), "same mesh revision is a no-op");
    const auto orange = solid(2, 2, 200, 100, 50, 255), half = solid(2, 2, 200, 100, 50, 128), mask = solid(2, 2, 255, 255, 255, 128);
    renderer.setTexture("srgb", 1, {2, 2, orange, TextureColorSpace::sRGB});
    renderer.setTexture("half", 1, {2, 2, half, TextureColorSpace::sRGB});
    renderer.setTexture("encoded", 1, {2, 2, orange, TextureColorSpace::encodedSRGB});
    renderer.setTexture("alpha", 1, {2, 2, mask, TextureColorSpace::sRGB});
    check(renderer.stats().resourceBytes > 0 && renderer.stats().textures == 4, "texture accounting");

    const auto at = [](const std::string& id, const std::string& texture, double x, double y) {
        DrawObject d; d.sourceID = id; d.meshID = "quad"; d.textureID = texture; d.world.values = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, 0, 1}; return d;
    };
    std::vector<DrawObject> list{at("a", "srgb", 0, 0), at("b", "half", 16, 0), at("c", "encoded", 32, 0), at("d", "alpha", 48, 0),
                                 at("e", "srgb", 0, 16), at("f", "srgb", 16, 16), at("g", "", 32, 16), at("h", "srgb", 48, 16)};
    list[4].linearTint = {.5f, .5f, .5f, 1};
    list[5].opacity = .5f;
    PlaneMask clip; clip.worldToLocal.values = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; clip.bounds = {32, 16, 8, 16};
    list[6].masks = {clip};
    list[7].angularMask = AngularMask{clip.worldToLocal, {56, 24}, 0, 0, std::nullopt}; // empty sweep
    renderer.setDrawList(list); renderer.draw(false);
    auto image = renderer.readback();
    expect(image, 8, 8, 200, 100, 50, 255, "opaque sRGB texture keeps its bytes");
    // Premultiplied in linear light, then re-premultiplied after encoding.
    const double a = 128 / 255.0;
    expect(image, 24, 8, 200 * a, 100 * a, 50 * a, 128, "half-transparent sRGB texture");
    expect(image, 40, 8, 200, 100, 50, 255, "encoded texture decodes for the linear target");
    expect(image, 56, 8, 255 * a, 255 * a, 255 * a, 128, "alpha-only texture reads as premultiplied white");
    const auto tinted = [](int c) { return 255 * encode(decode(c / 255.0) * .5); };
    expect(image, 8, 24, tinted(200), tinted(100), tinted(50), 255, "linear tint");
    expect(image, 24, 24, 100, 50, 25, 128, "object opacity");
    expect(image, 36, 24, 255, 255, 255, 255, "inside the plane mask");
    expect(image, 44, 24, 0, 0, 0, 0, "outside the plane mask");
    expect(image, 56, 24, 0, 0, 0, 0, "empty angular mask");
    expect(image, 8, 48, 0, 0, 0, 0, "untouched area stays transparent");

    // An encoded native group: half-transparent over opaque blends in encoded space.
    std::vector<DrawObject> inside{at("under", "", 0, 0), at("over", "half", 0, 0)};
    inside[0].linearTint = {0, 0, 0, 1};
    NativeGroupTarget target{{0, 0, 16, 16}, 1, NativeGroupColorSpace::encodedSRGB};
    check(renderer.configureNativeGroup("g", target, inside), "group configure");
    check(!renderer.configureNativeGroup("g", target, inside), "unchanged group is a no-op");
    auto output = renderer.nativeGroupOutput("g"); output.world.values = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 32, 0, 1};
    list.push_back(output); renderer.setDrawList(list); renderer.draw(false); image = renderer.readback();
    expect(image, 8, 40, 200 * a, 100 * a, 50 * a, 255, "encoded group blends in encoded space");
    check(renderer.stats().nativeGroupRenders == 1, "group rendered once");
    renderer.draw(false); check(renderer.stats().nativeGroupRenders == 1, "clean group is not re-rendered");

    check(!renderer.removeTexture("srgb"), "a texture in use cannot be removed");
    check(!renderer.removeNativeGroup("g"), "a published group cannot be removed");
    bool rejected = false; auto bad = list; bad[0].opacity = 2;
    try { renderer.setDrawList(bad); } catch (const std::exception&) { rejected = true; }
    check(rejected, "invalid opacity is rejected");
    renderer.draw(false); expect(renderer.readback(), 8, 8, 200, 100, 50, 255, "rejected list changed nothing");

    renderer.clearDrawList(); check(renderer.removeNativeGroup("g"), "group removal");
    auto parked = renderer.parkTexture("srgb");
    check(parked && parked->held() && parked->revision() == 1 && !renderer.textureRevision("srgb"), "park texture");
    check(parked && renderer.restoreTexture("srgb", *parked) && renderer.textureRevision("srgb") == 1u, "restore texture");

    const auto canvas = solid(4, 4, 255, 255, 255, 255), patch = solid(2, 2, 200, 100, 50, 255);
    TextureData blank{4, 4, canvas, TextureColorSpace::sRGB}; blank.updatable = true; blank.filter = TextureFilter::nearest;
    renderer.setTexture("paint", 1, blank);
    check(renderer.updateTexture("paint", 2, 0, 0, {2, 2, patch, TextureColorSpace::sRGB}), "paint a colour into a white texture");
    renderer.setDrawList(std::vector<DrawObject>{at("p", "paint", 0, 0)}); renderer.draw(false); image = renderer.readback();
    expect(image, 3, 3, 200, 100, 50, 255, "painted region"); expect(image, 13, 13, 255, 255, 255, 255, "unpainted region kept");

    std::printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::printf("exception: %s\n", error.what());
    return 2;
}
