// Linux implementation of endfield::native::LayerRasterizer (windows/native/layer_raster.hpp):
// local layer trees painted with Cairo, text shaped with Pango and the bundled fonts.
// Follows windows/native/layer_raster.cpp. Not implemented: editor documents (plain and rich
// text viewports, retained layouts, hit testing); textLayout() returns none.
#include "native/layer_raster.hpp"

#include "core/data/sha256.hpp"
#include "core/localization.hpp"
#include "core/notes_font_names.hpp"
#include "core/notes_rich_text.hpp"
#include "native/layer_image_source.hpp"
#include "native/notes_rich_style.hpp"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <hb.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <thread>

namespace endfield::native {

struct detail::LayerRasterRecipe {
    std::shared_ptr<const ehud::data::Json> layer;
    LayerRasterOptions options;
    LayerFontLanguage language{};
    std::size_t bytes{};
};

namespace {
using Json = ehud::data::Json;
using Matrix = cairo_matrix_t;

// Core Animation's shadow blur: a Gaussian of 0.9 x shadowRadius (as on Windows).
constexpr double shadowSigmaPerRadius = .9;

[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
double number(const Json& value, double fallback = 0) {
    if (value.isNull()) return fallback;
    if (!value.isNumber()) invalid("Layer number has the wrong type");
    const auto n = value.number();
    if (!std::isfinite(n) || std::abs(n) > 1e8) invalid("Layer number is nonfinite or outside its bounds");
    return n;
}
bool flag(const Json& value, bool fallback = false) {
    if (value.isNull()) return fallback;
    if (!value.isBool()) invalid("Layer boolean has the wrong type");
    return value.boolean();
}
std::string string(const Json& value, std::string fallback = {}) {
    if (value.isNull()) return fallback;
    if (!value.isString()) invalid("Layer string has the wrong type");
    std::string text(value.string()); if (text.size() > 262144) invalid("Layer string exceeds its limit"); return text;
}
const Json::Array& array(const Json& value, std::size_t maximum) {
    if (!value.isArray() || value.array().size() > maximum) invalid("Layer array exceeds its bounds");
    return value.array();
}
core::Rect rectangle(const Json& value) {
    const auto& a = array(value, 4); if (a.size() != 4) invalid("Layer rect requires four numbers");
    core::Rect r{number(a[0]), number(a[1]), number(a[2]), number(a[3])};
    if (r.width < 0 || r.height < 0) invalid("Visible layer bounds cannot be negative");
    return r;
}
core::Point point(const Json& value, core::Point fallback = {}) {
    if (value.isNull()) return fallback;
    const auto& a = array(value, 2); if (a.size() != 2) invalid("Layer point requires two numbers");
    return {number(a[0]), number(a[1])};
}
core::Rect unite(core::Rect a, core::Rect b) {
    const double x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.width, b.x + b.width) - x, std::max(a.y + a.height, b.y + b.height) - y};
}
Matrix identityMatrix() { Matrix m; cairo_matrix_init_identity(&m); return m; }
Matrix translation(double x, double y) { Matrix m; cairo_matrix_init_translate(&m, x, y); return m; }
// Row-vector order, as Direct2D: `first` applies before `second`.
Matrix then(const Matrix& first, const Matrix& second) { Matrix m; cairo_matrix_multiply(&m, &first, &second); return m; }
core::Point apply(const Matrix& m, core::Point p) { cairo_matrix_transform_point(&m, &p.x, &p.y); return p; }
double areaScale(const Matrix& m) { return std::sqrt(std::abs(m.xx * m.yy - m.xy * m.yx)); }
core::Rect transformBounds(core::Rect r, const Matrix& m) {
    const std::array<core::Point, 4> p{apply(m, {r.x, r.y}), apply(m, {r.x + r.width, r.y}), apply(m, {r.x + r.width, r.y + r.height}), apply(m, {r.x, r.y + r.height})};
    double left = p[0].x, right = left, top = p[0].y, bottom = top;
    for (const auto& v : p) { left = std::min(left, v.x); right = std::max(right, v.x); top = std::min(top, v.y); bottom = std::max(bottom, v.y); }
    return {left, top, right - left, bottom - top};
}
bool affine(const Json& value, Matrix& result) {
    if (value.isNull()) { result = identityMatrix(); return true; }
    const auto& columns = array(value, 4); if (columns.size() != 4) invalid("Layer transform requires four columns");
    double m[4][4]{};
    for (unsigned c = 0; c < 4; ++c) { const auto& values = array(columns[c], 4); if (values.size() != 4) invalid("Layer matrix column is incomplete"); for (unsigned r = 0; r < 4; ++r) m[c][r] = number(values[r]); }
    constexpr double epsilon = 1e-8;
    if (std::abs(m[0][2]) + std::abs(m[0][3]) + std::abs(m[1][2]) + std::abs(m[1][3]) + std::abs(m[2][0]) + std::abs(m[2][1]) + std::abs(m[2][3]) + std::abs(m[3][2]) > epsilon
        || std::abs(m[2][2] - 1) > epsilon || std::abs(m[3][3] - 1) > epsilon) return false;
    cairo_matrix_init(&result, m[0][0], m[0][1], m[1][0], m[1][1], m[3][0], m[3][1]);
    return true;
}
bool placement(const Json& node, Matrix& result) {
    Matrix transform; if (!affine(node["transform"], transform)) return false;
    const auto bounds = rectangle(node["bounds"]); const auto position = point(node["position"]), anchor = point(node["anchorPoint"], {.5, .5});
    result = then(then(translation(-bounds.x - anchor.x * bounds.width, -bounds.y - anchor.y * bounds.height), transform), translation(position.x, position.y));
    return true;
}
bool childTransform(const Json& node, Matrix& result) {
    Matrix sub; if (!affine(node["sublayerTransform"], sub)) return false;
    const auto bounds = rectangle(node["bounds"]); const auto anchor = point(node["anchorPoint"], {.5, .5});
    const double x = bounds.x + anchor.x * bounds.width, y = bounds.y + anchor.y * bounds.height;
    // Core Animation applies sublayerTransform about the parent's anchor.
    result = then(then(translation(-x, -y), sub), translation(x, y)); return true;
}
struct Color { double r{}, g{}, b{}, a{1}; };
bool color(const Json& value, Color& result, double opacity = 1) {
    if (value.isNull()) return false;
    const auto& c = array(value["sRGB"], 4); if (c.size() != 4) invalid("A layer color needs explicit converted sRGB RGBA");
    result = {std::clamp(number(c[0]), 0., 1.), std::clamp(number(c[1]), 0., 1.), std::clamp(number(c[2]), 0., 1.), std::clamp(number(c[3]), 0., 1.) * opacity};
    return true;
}
std::vector<const Json*> children(const Json& node) {
    std::vector<const Json*> result; if (node["children"].isNull()) return result;
    for (const auto& child : array(node["children"], LayerRasterizer::maximumNodes)) result.push_back(&child);
    std::stable_sort(result.begin(), result.end(), [](auto a, auto b) { return number((*a)["zPosition"]) < number((*b)["zPosition"]); });
    return result;
}
bool lineBox(const Json& descriptor) { return descriptor.isObject() && !descriptor["lineHeight"].isNull(); }
core::Point lineBoxPadding(const Json& descriptor) {
    double size = number(descriptor["fontSize"], 12);
    if (descriptor["runs"].isArray()) for (const auto& run : descriptor["runs"].array()) {
        const auto& a = run["attributes"]; if (!a.isObject()) continue;
        if (a["fontSize"].isNumber()) size = std::max(size, a["fontSize"].number());
        if (a["NSFont"].isObject() && a["NSFont"]["pointSize"].isNumber()) size = std::max(size, a["NSFont"]["pointSize"].number());
    }
    return {std::ceil(size * .25), std::ceil(size * .45)};
}
bool containsText(const Json& node) { if (node["text"].isObject()) return true; if (node["children"].isArray()) for (const auto& child : node["children"].array()) if (containsText(child)) return true; return false; }
double rasterDensity(const Json& layer, const LayerRasterOptions& o) {
    if (!o.textContentsScale) return o.pixelsPerPoint; double density = o.pixelsPerPoint;
    const double cap = core::maximumRenderScale * core::renderMagnification(o.magnification);
    const auto visit = [&](const Json& node, unsigned depth, const auto& self) -> void {
        if (depth > 64 || flag(node["hidden"])) return;
        if (node["text"].isObject() && node["contentsScale"].isNumber() && std::isfinite(node["contentsScale"].number())) density = std::max(density, std::min(cap, node["contentsScale"].number()));
        if (node["children"].isArray()) for (const auto& child : node["children"].array()) self(child, depth + 1, self);
    };
    visit(layer, 0, visit); return density;
}
// Estimated size of a retained description; false once it exceeds `limit`.
bool jsonBytesWithin(const Json& v, std::size_t limit, std::size_t& total, unsigned depth = 0) {
    const auto add = [&](std::size_t n) { if (n > limit - total) return false; total += n; return true; };
    if (depth > 128 || !add(sizeof(Json))) return false;
    if (v.isString()) return add(v.string().size() + 1);
    if (v.isArray()) { if (!add(v.array().size() * sizeof(Json))) return false; for (const auto& a : v.array()) if (!jsonBytesWithin(a, limit, total, depth + 1)) return false; return true; }
    if (v.isObject()) for (const auto& [k, a] : v.object()) if (!add(sizeof(Json) + 4 * sizeof(void*) + k.size() + 1) || !jsonBytesWithin(a, limit, total, depth + 1)) return false;
    return true;
}

struct SurfaceDeleter { void operator()(cairo_surface_t* s) const { cairo_surface_destroy(s); } };
struct CairoDeleter { void operator()(cairo_t* c) const { cairo_destroy(c); } };
struct ObjectDeleter { void operator()(gpointer p) const { if (p) g_object_unref(p); } };
using Surface = std::unique_ptr<cairo_surface_t, SurfaceDeleter>;
using Context = std::unique_ptr<cairo_t, CairoDeleter>;
using Layout = std::unique_ptr<PangoLayout, ObjectDeleter>;

Surface newSurface(unsigned width, unsigned height) {
    Surface s(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(width), int(height)));
    if (cairo_surface_status(s.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error("Cannot create a bounded local bitmap");
    return s;
}
Context newContext(cairo_surface_t* s) { Context c(cairo_create(s)); cairo_set_antialias(c.get(), CAIRO_ANTIALIAS_GRAY); return c; }
void source(cairo_t* cr, const Color& c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void roundedRect(cairo_t* cr, core::Rect r, double radius) {
    radius = std::min({radius, r.width / 2, r.height / 2});
    if (radius <= 0) { cairo_rectangle(cr, r.x, r.y, r.width, r.height); return; }
    constexpr double q = std::numbers::pi / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.width - radius, r.y + radius, radius, -q, 0);
    cairo_arc(cr, r.x + r.width - radius, r.y + r.height - radius, radius, 0, q);
    cairo_arc(cr, r.x + radius, r.y + r.height - radius, radius, q, 2 * q);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, 2 * q, 3 * q);
    cairo_close_path(cr);
}
void boundsPath(cairo_t* cr, const Json& node) { cairo_new_path(cr); roundedRect(cr, rectangle(node["bounds"]), std::max(0., number(node["cornerRadius"]))); }
void commandPath(cairo_t* cr, const Json& commands, const std::string& fillRule) {
    cairo_new_path(cr);
    cairo_set_fill_rule(cr, fillRule == "even-odd" || fillRule == "evenOdd" ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
    bool open = false; core::Point last{}, start{};
    if (!commands.isNull()) for (const auto& command : array(commands, 65536)) {
        const auto op = string(command["op"]); const auto& p = array(command["points"], 3);
        if (op == "move") { if (p.size() != 1) invalid("Invalid move command"); last = start = point(p[0]); cairo_move_to(cr, last.x, last.y); open = true; }
        else if (op == "line") { if (!open || p.size() != 1) invalid("Invalid line command"); last = point(p[0]); cairo_line_to(cr, last.x, last.y); }
        else if (op == "quadratic") {
            if (!open || p.size() != 2) invalid("Invalid quadratic command"); const auto c = point(p[0]), e = point(p[1]);
            cairo_curve_to(cr, last.x + 2 * (c.x - last.x) / 3, last.y + 2 * (c.y - last.y) / 3, e.x + 2 * (c.x - e.x) / 3, e.y + 2 * (c.y - e.y) / 3, e.x, e.y); last = e;
        } else if (op == "cubic") { if (!open || p.size() != 3) invalid("Invalid cubic command"); const auto a = point(p[0]), b = point(p[1]); last = point(p[2]); cairo_curve_to(cr, a.x, a.y, b.x, b.y, last.x, last.y); }
        else if (op == "close") { if (!open || !p.empty()) invalid("Invalid close command"); cairo_close_path(cr); open = false; last = start; }
        else invalid("Unknown retained path command");
    }
}
void strokeStyle(cairo_t* cr, const Json& shape, double width) {
    const auto cap = string(shape["lineCap"], "butt"), join = string(shape["lineJoin"], "miter");
    cairo_set_line_cap(cr, cap == "round" ? CAIRO_LINE_CAP_ROUND : cap == "square" ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, join == "round" ? CAIRO_LINE_JOIN_ROUND : join == "bevel" ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(cr, std::max(1., number(shape["miterLimit"], 10)));
    cairo_set_line_width(cr, width);
    std::vector<double> dashes;
    if (!shape["lineDashPattern"].isNull()) for (const auto& v : array(shape["lineDashPattern"], 256)) { const double d = number(v); if (d < 0) invalid("Negative dash length"); dashes.push_back(d); }
    if (!dashes.empty() && std::all_of(dashes.begin(), dashes.end(), [](double v) { return v == 0; })) invalid("Zero-length dash pattern");
    cairo_set_dash(cr, dashes.data(), int(dashes.size()), number(shape["lineDashPhase"]));
}

// Three box passes approximate a Gaussian of the given standard deviation.
void blurAlpha(std::vector<float>& a, unsigned w, unsigned h, double sigma) {
    if (sigma < .3) return;
    const int radius = std::max(1, int(std::lround((std::sqrt(4 * sigma * sigma + 1) - 1) / 2)));
    std::vector<float> line(std::max(w, h));
    const auto pass = [&](float* data, unsigned count, std::size_t stride) {
        double sum = 0; const int n = int(count);
        for (int i = 0; i <= radius && i < n; ++i) sum += data[i * stride];
        for (int i = 0; i < n; ++i) {
            line[i] = float(sum / (2 * radius + 1));
            if (i + radius + 1 < n) sum += data[(i + radius + 1) * stride];
            if (i - radius >= 0) sum -= data[(i - radius) * stride];
        }
        for (int i = 0; i < n; ++i) data[i * stride] = line[i];
    };
    for (int k = 0; k < 3; ++k) {
        for (unsigned y = 0; y < h; ++y) pass(a.data() + std::size_t(y) * w, w, 1);
        for (unsigned x = 0; x < w; ++x) pass(a.data() + x, h, w);
    }
}

struct ResolvedFont { std::string family, list; bool standard{}; };
struct FontSpec { ResolvedFont font; PangoWeight weight{PANGO_WEIGHT_NORMAL}; bool italic{}; double size{12}; };
PangoWeight weightOf(const Json& font) {
    const auto traits = static_cast<unsigned>(number(font["symbolicTraits"])); const auto name = string(font["postScriptName"]);
    const auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (name == ".AppleSystemUIFontDemi" || has("Semibold") || has("SemiBold")) return PANGO_WEIGHT_SEMIBOLD;
    if (has("Black") || has("Heavy")) return PANGO_WEIGHT_HEAVY;
    if ((traits & 2) || has("Bold")) return PANGO_WEIGHT_BOLD;
    if (has("Medium")) return PANGO_WEIGHT_MEDIUM;
    if (has("Light")) return PANGO_WEIGHT_LIGHT;
    if (has("Thin")) return PANGO_WEIGHT_THIN;
    return PANGO_WEIGHT_NORMAL;
}
// The five HarmonyOS Sans faces the standard family offers.
PangoWeight standardWeight(int w) {
    return PangoWeight(w < 350 ? 300 : w < 450 ? 400 : w < 550 ? 500 : w < 750 ? 700 : 900);
}
PangoWeight textWeight(const Json& style, const Json& font, bool standard) {
    int w = weightOf(font);
    if (!style["fontWeight"].isNull()) { const auto value = number(style["fontWeight"]); if (value < 1 || value > 999) invalid("Text weight exceeds its range"); w = int(std::lround(value)); }
    return standard ? standardWeight(w) : PangoWeight(w);
}
PangoFontDescription* description(const FontSpec& s) {
    PangoFontDescription* d = pango_font_description_new();
    pango_font_description_set_family(d, s.font.list.c_str());
    pango_font_description_set_weight(d, s.weight);
    pango_font_description_set_style(d, s.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(d, s.size * PANGO_SCALE);
    return d;
}
// Maps UTF-16 offsets (the descriptor's unit) to UTF-8 byte offsets.
std::vector<std::size_t> utf16Offsets(const std::string& text) {
    std::vector<std::size_t> map;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        map.push_back(i); if (n == 4) map.push_back(i);
        i += n;
    }
    map.push_back(text.size()); return map;
}
std::string fromUtf16(std::u16string_view value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        std::uint32_t c = value[i];
        if (c >= 0xd800 && c <= 0xdbff) { if (++i == value.size() || value[i] < 0xdc00 || value[i] > 0xdfff) invalid("Text has an unmatched UTF-16 surrogate"); c = 0x10000 + ((c - 0xd800) << 10) + (value[i] - 0xdc00); }
        else if (c >= 0xdc00 && c <= 0xdfff) invalid("Text has an unmatched UTF-16 surrogate");
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
        else { out += char(0xf0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    return out;
}
struct RunMetric { std::size_t begin{}, end{}; std::array<double, 3> em{}; double size{}; };
} // namespace

struct LayerPlainTextAnalysis::Impl {
    std::thread::id thread{std::this_thread::get_id()};
    LayerPlainTextMetrics metrics;
    std::vector<std::uint32_t> lengths;
    std::vector<LayerStyledTextLine> styledLines;
    std::function<Layout(const std::string&, double width, std::span<const core::notes::TextRun>)> shape;
    std::uint64_t created{};
    void onThread() const { if (std::this_thread::get_id() != thread) throw std::logic_error("Plain text analysis used outside its creating thread"); }
};

struct LayerRasterizer::Impl {
    using Recipe = detail::LayerRasterRecipe;
    struct Entry { std::uint64_t revision; LayerRasterOptions options; std::shared_ptr<LayerRasterImage> image; std::uint64_t fontRevision{}; bool hasText{}; std::shared_ptr<const Recipe> source; std::size_t descriptorBytes{}; std::string owner; bool releasable{}; };
    struct DecodedImage { unsigned width{}, height{}; std::vector<std::uint8_t> premultipliedBGRA; };
    struct ImagePixels { unsigned width{}, height{}; std::span<const std::uint8_t> premultipliedBGRA; std::shared_ptr<const LayerMemoryImage> borrowed; };
    std::thread::id thread{std::this_thread::get_id()};
    std::map<std::string, Entry, std::less<>> entries;
    std::map<std::string, DecodedImage, std::less<>> images;
    LayerRasterStats counts; LayerFontLanguage language{LayerFontLanguage::simplifiedChinese}; std::uint64_t fontRevision{1};
    std::size_t releasedBytes{};
    std::size_t heldBytes() const noexcept { return counts.resourceBytes - releasedBytes; }
    static constexpr std::size_t heldBound = 1;
    static constexpr std::array<std::size_t, 4> maxima{maximumEntries, maximumResourceBytes, maximumTextMetadataBytes, maximumTypographyDescriptorBytes};
    std::array<std::size_t, 4> load() const noexcept { return {entries.size(), heldBytes(), counts.textMetadataBytes, counts.typographyDescriptorBytes}; }
    unsigned over(std::size_t eighths) const noexcept { const auto used = load(); unsigned bits{}; for (std::size_t n = 0; n < 4; ++n) if (used[n] > maxima[n] / 8 * eighths) bits |= 1u << n; return bits; }
    bool above(std::size_t eighths) const noexcept { return over(eighths) != 0; }
    struct Tenant { LayerRasterTenant* page{}; std::uint64_t left{}; LayerRasterTenant::State state{}; };
    std::vector<Tenant> tenants; std::uint64_t leavings{}; unsigned futile{};
    bool memoryImageUsed{}, memoryImageMissing{};

    struct BundledFamily { std::string name; bool latin{}; std::optional<LayerFontLanguage> region; };
    std::vector<BundledFamily> bundled; std::vector<std::string> bundledNames; std::string latinFamily;
    std::array<std::string, 4> regionFamilies;
    std::set<std::string> available; // every family the font map offers
    std::optional<std::vector<std::string>> installedFontNames;
    std::map<std::string, std::array<double, 3>> struts;
    FcConfig* fontConfig{};
    std::unique_ptr<PangoFontMap, ObjectDeleter> fontMap;
    std::unique_ptr<PangoContext, ObjectDeleter> pango;
    Surface scratchSurface; Context scratch;

    static std::size_t index(LayerFontLanguage l) noexcept { return static_cast<std::size_t>(l); }
    std::string standardFamily() const { return latinFamily.empty() ? "sans-serif" : latinFamily; }
    std::string regionFamily(LayerFontLanguage l) const { const auto& f = regionFamilies[index(l)]; return f.empty() ? "sans-serif" : f; }
    std::string fallbackFamily(const LayerRasterOptions& o) const { return o.fallbackFontFamily.empty() || !available.contains(o.fallbackFontFamily) ? standardFamily() : o.fallbackFontFamily; }
    LayerFontLanguage region(const Json& descriptor) const {
        if (descriptor.isObject() && descriptor["language"].isString()) if (const auto l = core::regionFromTag(std::string(descriptor["language"].string()))) return *l;
        return language;
    }
    // Comma-separated Pango family list: the face, then the CJK cascade of the region.
    std::string cascade(const std::string& family, LayerFontLanguage preferred) const {
        std::string list = family;
        if (family != standardFamily()) return list;
        list += "," + regionFamily(preferred);
        for (const auto other : core::textRegions) if (other != preferred && !regionFamilies[index(other)].empty()) list += "," + regionFamilies[index(other)];
        return list;
    }
    std::optional<std::string> bundledFace(std::string_view name) const {
        const auto key = [](std::string_view v) { std::string k; for (const char c : v.substr(0, v.find('-'))) if (c != ' ' && c != '_') k.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c); return k; };
        const auto wanted = key(name); if (wanted.empty()) return std::nullopt;
        for (const auto& f : bundled) { const auto k = key(f.name); if (f.latin ? wanted.starts_with(k) : wanted == k) return f.name; }
        return std::nullopt;
    }
    static std::string sha256(std::span<const std::uint8_t> bytes) { ehud::data::Sha256 s; s.update(bytes.data(), bytes.size()); return s.hex(); }

    void loadFonts(const std::filesystem::path& directory) {
        const auto manifestFile = directory / "manifest.json"; std::error_code error;
        const auto size = std::filesystem::file_size(manifestFile, error);
        if (error || size == 0 || size > 1024 * 1024) invalid("Bundled font manifest is missing");
        std::string data(std::size_t(size), '\0'); std::ifstream input(manifestFile, std::ios::binary);
        if (!input.read(data.data(), std::streamsize(data.size()))) invalid("Cannot read the bundled font manifest");
        const auto manifest = Json::parse(data, 1024 * 1024);
        if (!manifest["schemaVersion"].isNumber() || manifest["schemaVersion"].number() != 2) invalid("Bundled font manifest has an unknown schema");
        for (const auto& family : array(manifest["families"], 64)) {
            const auto name = string(family["family"]), role = string(family["role"]); bool listed = false;
            // The Linux build uses the files the Windows build ships.
            for (const auto& platform : array(family["platforms"], 16)) listed = listed || (platform.isString() && std::string(platform.string()) == "windows");
            if (name.empty() || !listed || (role != "latin" && role != "cjk")) continue;
            std::vector<std::filesystem::path> files; bool present = true;
            for (const auto& entry : array(family["files"], 64)) {
                const auto file = string(entry["file"]);
                if (!file.ends_with(".ttf") && !file.ends_with(".otf")) continue;
                if (file.find_first_of("/\\:") != std::string::npos || file.starts_with(".")) invalid("Bundled font manifest names a file outside fonts/");
                const auto path = directory / file; const auto bytes = std::uintmax_t(number(entry["bytes"]));
                if (!std::filesystem::is_regular_file(path, error)) { present = false; continue; }
                if (std::filesystem::file_size(path) != bytes) invalid("Bundled font has an unexpected size");
                if (bytes <= 1024 * 1024) {
                    std::vector<std::uint8_t> content(std::size_t(bytes), 0); std::ifstream font(path, std::ios::binary);
                    if (!font.read(reinterpret_cast<char*>(content.data()), std::streamsize(content.size())) || sha256(content) != string(entry["sha256"])) invalid("Bundled font does not match its manifest SHA-256");
                }
                files.push_back(path);
            }
            if (files.empty() || !present) continue;
            for (const auto& f : files) if (!FcConfigAppFontAddFile(fontConfig, reinterpret_cast<const FcChar8*>(f.c_str()))) invalid("Cannot add a bundled font");
            BundledFamily loaded{name, role == "latin", {}};
            if (!loaded.latin && !family["languages"].isNull()) for (const auto& tag : array(family["languages"], 16)) if (tag.isString()) if (const auto r = core::regionFromTag(std::string(tag.string()))) { loaded.region = r; break; }
            if (loaded.latin && latinFamily.empty()) latinFamily = name;
            if (loaded.region && regionFamilies[index(*loaded.region)].empty()) regionFamilies[index(*loaded.region)] = name;
            if (std::find(bundledNames.begin(), bundledNames.end(), name) == bundledNames.end()) bundledNames.push_back(name);
            bundled.push_back(std::move(loaded));
        }
    }

    explicit Impl(std::filesystem::path directory) {
        if (directory.empty()) directory = std::filesystem::canonical("/proc/self/exe").parent_path() / "fonts";
        // A private configuration: bundled fonts are visible to this process only.
        fontConfig = FcInitLoadConfigAndFonts();
        if (!fontConfig) throw std::runtime_error("Cannot read the font configuration");
        loadFonts(directory);
        fontMap.reset(pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT));
        pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(fontMap.get()), fontConfig);
        pango.reset(pango_font_map_create_context(fontMap.get()));
        cairo_font_options_t* options = cairo_font_options_create();
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
        pango_cairo_context_set_font_options(pango.get(), options);
        cairo_font_options_destroy(options);
        pango_context_set_round_glyph_positions(pango.get(), FALSE);
        PangoFontFamily** families{}; int count{};
        pango_font_map_list_families(fontMap.get(), &families, &count);
        for (int i = 0; i < count; ++i) available.insert(pango_font_family_get_name(families[i]));
        g_free(families);
        for (const auto& f : bundled) if (!available.contains(f.name)) invalid("Bundled font has an unexpected family");
        scratchSurface = newSurface(1, 1); scratch = newContext(scratchSurface.get());
    }
    ~Impl() { pango.reset(); fontMap.reset(); if (fontConfig) FcConfigDestroy(fontConfig); }
    void onThread() const { if (std::this_thread::get_id() != thread) throw std::logic_error("Layer rasterizer used outside its creating thread"); }
    static void issue(LayerRasterImage& image, const Json& node, std::string feature) {
        const auto id = string(node["id"], "unnamed");
        if (std::none_of(image.unsupported.begin(), image.unsupported.end(), [&](const auto& item) { return item.node == id && item.feature == feature; }))
            image.unsupported.push_back({id, std::move(feature)});
    }

    ResolvedFont fontFamily(const Json& font, const Json& node, const LayerRasterOptions& options, LayerRasterImage& result) {
        const auto family = string(font["familyName"], string(font["postScriptName"]));
        const auto postscript = string(font["postScriptName"]);
        const bool preserve = flag(font["preserveUserFont"]);
        const auto preferred = region(node["text"]);
        const auto make = [&](const std::string& selected) { return ResolvedFont{selected, cascade(selected, preferred), selected == standardFamily()}; };
        const auto report = [&](const std::string& selected) {
            LayerFontSubstitution s{string(node["id"], "unnamed"), family, postscript, selected, selected == standardFamily() ? regionFamily(preferred) : std::string{}};
            if (std::none_of(result.fontSubstitutions.begin(), result.fontSubstitutions.end(), [&](const auto& f) { return f.node == s.node && f.requestedFamily == family && f.requestedFace == postscript && f.selectedFamily == selected; })) result.fontSubstitutions.push_back(std::move(s));
        };
        const auto requested = preserve ? family : fallbackFamily(options);
        bool exists = !requested.empty() && available.contains(requested);
        if (!exists && preserve) {
            if (const auto face = bundledFace(postscript.empty() ? family : postscript)) { report(*face); return make(*face); }
            if (const auto mapped = core::notes::notesWindowsFontFamily(postscript.empty() ? family : postscript))
                if (const std::string name(*mapped); available.contains(name)) { report(name); return make(name); }
        }
        if (!exists) {
            const bool fixedPitch = (static_cast<unsigned>(number(font["symbolicTraits"])) & (1u << 10)) != 0 || family == "Menlo" || family == "Monaco" || family == "SF Mono" ||
                family == ".AppleSystemUIFontMonospaced" || family.starts_with(".SFNSMono") || postscript.starts_with("Menlo-") || postscript.starts_with("SFMono-");
            const std::string substitute = fixedPitch ? (available.contains(options.monospaceFallbackFontFamily) ? options.monospaceFallbackFontFamily : "monospace") : fallbackFamily(options);
            report(substitute); return make(substitute);
        }
        if (requested != family) report(requested);
        return make(requested);
    }

    // Line metrics in em from the face's hhea table: ascent, descent, line gap.
    std::array<double, 3> lineMetrics(const ResolvedFont& font, PangoWeight weight, bool italic) {
        const std::string key = font.family + "/" + std::to_string(weight) + (italic ? "i" : "");
        if (const auto found = struts.find(key); found != struts.end()) return found->second;
        FontSpec spec{{font.family, font.family, font.standard}, weight, italic, 100};
        PangoFontDescription* d = description(spec);
        std::unique_ptr<PangoFont, ObjectDeleter> loaded(pango_font_map_load_font(fontMap.get(), pango.get(), d));
        pango_font_description_free(d);
        if (!loaded) throw std::runtime_error("Cannot match a font face");
        hb_face_t* face = hb_font_get_face(pango_font_get_hb_font(loaded.get()));
        const double em = hb_face_get_upem(face);
        hb_blob_t* table = hb_face_reference_table(face, HB_TAG('h', 'h', 'e', 'a'));
        unsigned length{}; const auto* b = reinterpret_cast<const std::uint8_t*>(hb_blob_get_data(table, &length));
        std::array<double, 3> result{.93, .25, 0};
        if (length >= 10 && em > 0) {
            const auto s16 = [&](unsigned at) { return static_cast<std::int16_t>((b[at] << 8) | b[at + 1]); };
            result = {s16(4) / em, -s16(6) / em, std::max(0, int(s16(8))) / em};
        }
        hb_blob_destroy(table);
        return struts.emplace(key, result).first->second;
    }

    static void add(PangoAttrList* list, PangoAttribute* a, std::size_t begin, std::size_t end) {
        a->start_index = guint(begin); a->end_index = guint(end); pango_attr_list_insert(list, a);
    }
    // tabularFigures (OpenType tnum) and tracking in em, per range.
    void textFeatures(PangoAttrList* list, const Json& style, std::size_t begin, std::size_t end, double size) {
        if (!style["tabularFigures"].isNull()) add(list, pango_attr_font_features_new(flag(style["tabularFigures"]) ? "tnum=1" : "tnum=0"), begin, end);
        if (!style["tracking"].isNull()) {
            const auto tracking = number(style["tracking"]); if (std::abs(tracking) > 4) invalid("Text tracking exceeds its range");
            add(list, pango_attr_letter_spacing_new(int(std::lround(tracking * size * PANGO_SCALE))), begin, end);
        }
    }
    // The descriptor's runs applied to the part [from, to) (byte offsets) of the string.
    void applyRuns(PangoAttrList* list, const Json& node, const LayerRasterOptions& options, LayerRasterImage& result, double opacity,
                   const ResolvedFont& base, double size, double scale, const std::vector<std::size_t>& offsets, std::size_t from, std::size_t to,
                   std::vector<RunMetric>& metrics, bool colors) {
        const auto& descriptor = node["text"]; if (descriptor["runs"].isNull()) return;
        const std::size_t length = offsets.size() - 1;
        for (const auto& run : array(descriptor["runs"], 50000)) {
            const auto& r = array(run["utf16Range"], 2); if (r.size() != 2) invalid("Invalid UTF-16 run range");
            const double start = number(r[0]), runLength = number(r[1]);
            if (start < 0 || runLength < 0 || std::floor(start) != start || std::floor(runLength) != runLength || start + runLength > double(length)) invalid("Text run exceeds UTF-16 string");
            const auto& attributes = run["attributes"]; if (!attributes.isObject()) invalid("Text attributes must be an object");
            const auto first = std::max(offsets[std::size_t(start)], from), last = std::min(offsets[std::size_t(start + runLength)], to);
            if (last <= first) continue;
            const auto begin = first - from, end = last - from;
            const bool hasFont = attributes["NSFont"].isObject();
            const auto font = hasFont ? fontFamily(attributes["NSFont"], node, options, result) : base;
            double runSize = size; bool spaced = false, sized = false;
            for (const auto& [key, item] : attributes.object()) {
                if (key == "NSFont") {
                    runSize = number(item["pointSize"], size); if (runSize <= 0 || runSize > 2048) invalid("Run text size exceeds its range"); sized = true;
                    const bool italic = (static_cast<unsigned>(number(item["symbolicTraits"])) & 1) != 0;
                    const auto weight = textWeight(attributes, item, font.standard);
                    add(list, pango_attr_family_new(font.list.c_str()), begin, end);
                    add(list, pango_attr_size_new_absolute(int(std::lround(runSize * scale * PANGO_SCALE))), begin, end);
                    add(list, pango_attr_weight_new(weight), begin, end);
                    add(list, pango_attr_style_new(italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL), begin, end);
                    metrics.push_back({begin, end, lineMetrics(font, weight, italic), runSize * scale});
                } else if (key == "NSColor") {
                    if (!colors) continue;
                    Color c; if (!color(item, c, opacity)) continue;
                    add(list, pango_attr_foreground_new(guint16(std::lround(c.r * 65535)), guint16(std::lround(c.g * 65535)), guint16(std::lround(c.b * 65535))), begin, end);
                    // Pango reads an alpha of zero as unset.
                    add(list, pango_attr_foreground_alpha_new(guint16(std::max<long>(1, std::lround(c.a * 65535)))), begin, end);
                } else if (key == "NSUnderline" || key == "NSStrikethrough") {
                    const auto style = number(item); if (style != 0 && style != 1) issue(result, node, "unsupported decorated text style: " + std::string(key));
                    add(list, key == "NSUnderline" ? pango_attr_underline_new(style != 0 ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE) : pango_attr_strikethrough_new(style != 0), begin, end);
                } else if (key == "NSKern") { add(list, pango_attr_letter_spacing_new(int(std::lround(number(item) * scale * PANGO_SCALE))), begin, end); spaced = true; }
                else if (key == "NSLanguage") {
                    const auto tag = string(item);
                    if (core::regionFromTag(tag)) add(list, pango_attr_language_new(pango_language_from_string(tag.c_str())), begin, end);
                    else issue(result, node, "unsupported text language: " + tag);
                } else if (key == "fontSize") {
                    runSize = number(item); if (runSize <= 0 || runSize > 2048) invalid("Run text size exceeds its range"); sized = true;
                    add(list, pango_attr_size_new_absolute(int(std::lround(runSize * scale * PANGO_SCALE))), begin, end);
                    metrics.push_back({begin, end, lineMetrics(base, textWeight(attributes, Json{}, base.standard), false), runSize * scale});
                } else if (key == "fontWeight") { if (!hasFont) add(list, pango_attr_weight_new(textWeight(attributes, Json{}, font.standard)), begin, end); }
                else if (key == "tabularFigures" || key == "tracking") { spaced = spaced || key == "tracking"; }
                else if (key == "NSBaselineOffset" || key == "NSObliqueness" || key == "NSExpansion" || key == "NSStrokeWidth") { if (number(item) != 0) issue(result, node, "unsupported text run effect: " + std::string(key)); }
                else if (key == "NSLigature") { if (number(item) != 1) issue(result, node, "nondefault text ligature setting"); }
                else issue(result, node, "unsupported text attribute: " + std::string(key));
            }
            textFeatures(list, attributes, begin, end, runSize * scale);
            if (sized && !spaced && !descriptor["tracking"].isNull()) textFeatures(list, Json::Object{{"tracking", descriptor["tracking"]}}, begin, end, runSize * scale);
        }
    }

    struct Shaped { Layout layout; FontSpec base; std::vector<RunMetric> runs; };
    // A layout of bytes [from, to) of the descriptor's string. `width` below zero means unbounded.
    Shaped shape(const Json& node, const std::string& value, const std::vector<std::size_t>& offsets, std::size_t from, std::size_t to,
                 const FontSpec& base, double scale, double width, bool oneLine, const LayerRasterOptions& options, LayerRasterImage& result, double opacity, bool colors) {
        const auto& descriptor = node["text"];
        Shaped out; out.base = base; out.base.size = base.size * scale;
        std::string part = value.substr(from, to - from);
        if (oneLine) { // A single line never breaks at its own separators; they draw as spaces.
            for (char& c : part) if (c == '\n' || c == '\r' || c == '\v' || c == '\f') c = ' ';
        }
        out.layout.reset(pango_layout_new(pango.get()));
        PangoLayout* l = out.layout.get();
        PangoFontDescription* d = description(out.base); pango_layout_set_font_description(l, d); pango_font_description_free(d);
        pango_layout_set_single_paragraph_mode(l, oneLine);
        pango_layout_set_auto_dir(l, TRUE);
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
        if (width >= 0) pango_layout_set_width(l, int(std::lround(width * PANGO_SCALE)));
        pango_layout_set_text(l, part.data(), int(part.size()));
        PangoAttrList* list = pango_attr_list_new();
        pango_attr_list_insert(list, pango_attr_language_new(pango_language_from_string(std::string(core::regionTag(region(descriptor))).c_str())));
        textFeatures(list, descriptor, 0, part.size(), out.base.size);
        applyRuns(list, node, options, result, opacity, base.font, base.size, scale, offsets, from, to, out.runs, colors);
        pango_layout_set_attributes(l, list); pango_attr_list_unref(list);
        ++counts.textLayoutsCreated;
        return out;
    }
    static double layoutWidth(PangoLayout* l) { PangoRectangle logical; pango_layout_get_extents(l, nullptr, &logical); return double(logical.width) / PANGO_SCALE; }

    struct Line { PangoLayoutLine* line{}; double x{}, ascent{}, height{}; std::size_t begin{}, length{}; };
    // Lines with natural metrics (the largest font on each line), or the uniform pair given.
    std::vector<Line> lines(const Shaped& s, std::optional<std::pair<double, double>> uniform) {
        std::vector<Line> result;
        const auto baseMetric = lineMetrics(s.base.font, s.base.weight, s.base.italic);
        PangoLayoutIter* it = pango_layout_get_iter(s.layout.get());
        do {
            Line line; line.line = pango_layout_iter_get_line_readonly(it);
            PangoRectangle logical; pango_layout_iter_get_line_extents(it, nullptr, &logical);
            line.x = double(logical.x) / PANGO_SCALE;
            line.begin = std::size_t(pango_layout_line_get_start_index(line.line)); line.length = std::size_t(pango_layout_line_get_length(line.line));
            if (uniform) { line.height = uniform->first; line.ascent = uniform->second; }
            else {
                double a = 0, d = 0, g = 0; bool covered = false;
                for (const auto& run : s.runs) if (run.begin < line.begin + std::max<std::size_t>(1, line.length) && run.end > line.begin) {
                    a = std::max(a, run.em[0] * run.size); d = std::max(d, run.em[1] * run.size); g = std::max(g, run.em[2] * run.size);
                    covered = covered || (run.begin <= line.begin && run.end >= line.begin + line.length);
                }
                if (!covered) { a = std::max(a, baseMetric[0] * s.base.size); d = std::max(d, baseMetric[1] * s.base.size); g = std::max(g, baseMetric[2] * s.base.size); }
                line.ascent = a; line.height = a + d + g;
            }
            result.push_back(line);
        } while (pango_layout_iter_next_line(it));
        pango_layout_iter_free(it);
        return result;
    }
    static void paintLines(cairo_t* cr, const std::vector<Line>& list, double x, double y) {
        for (const auto& line : list) {
            cairo_move_to(cr, x + line.x, y + line.ascent);
            pango_cairo_show_layout_line(cr, line.line);
            y += line.height;
        }
        cairo_new_path(cr);
    }

    FontSpec baseFont(const Json& node, const LayerRasterOptions& options, LayerRasterImage& result, double size) {
        const auto& descriptor = node["text"];
        FontSpec spec; spec.font = fontFamily(descriptor["font"], node, options, result);
        spec.italic = (static_cast<unsigned>(number(descriptor["font"]["symbolicTraits"])) & 1) != 0;
        spec.weight = textWeight(descriptor, descriptor["font"], spec.font.standard); spec.size = size; return spec;
    }

    struct LineBoxText { std::vector<std::pair<Shaped, std::vector<Line>>> parts; std::vector<core::Point> origins; double scale{1}, natural{}, lineHeight{}; std::size_t count{}; };
    LineBoxText lineBoxText(const Json& node, const std::string& value, core::Rect box, const LayerRasterOptions& options, LayerRasterImage& result, double opacity, bool colors) {
        const auto& descriptor = node["text"]; LineBoxText out;
        const auto multiple = number(descriptor["lineHeight"]); if (!(multiple > 0 && multiple <= 8)) invalid("Line height exceeds its range");
        const auto maximum = number(descriptor["maximumLines"], 1); if (maximum < 0 || maximum > 100000 || std::floor(maximum) != maximum) invalid("Invalid maximum line count");
        const auto minimumScale = number(descriptor["minimumScale"], 1); if (!(minimumScale > 0 && minimumScale <= 1)) invalid("Invalid minimum text scale");
        const auto truncation = string(descriptor["truncation"], "end"); if (truncation != "end" && truncation != "none") issue(result, node, "unsupported text truncation: " + truncation);
        const auto alignment = string(descriptor["alignment"], "left"); if (alignment != "left" && alignment != "center" && alignment != "right") issue(result, node, "unsupported line-box alignment: " + alignment);
        const auto size = number(descriptor["fontSize"], 12); if (size <= 0 || size > 2048) invalid("Text size exceeds its range");
        const auto base = baseFont(node, options, result, size);
        const auto metrics = lineMetrics(base.font, base.weight, base.italic);
        const bool single = maximum == 1, truncate = truncation == "end";
        const auto offsets = utf16Offsets(value);
        const auto spacing = [&](double scale) {
            out.lineHeight = size * scale * multiple; const double ascent = metrics[0] * size * scale, descent = metrics[1] * size * scale;
            return std::pair{out.lineHeight, (out.lineHeight - ascent - descent) / 2 + ascent};
        };
        const auto addPart = [&](Shaped s, double x, double y) { auto l = lines(s, spacing(out.scale)); out.origins.push_back({x, y}); out.parts.emplace_back(std::move(s), std::move(l)); };
        // One line from `from`, ellipsized to the box, placed by its width.
        const auto line = [&](std::size_t from, double scale, double y) {
            auto s = shape(node, value, offsets, from, value.size(), base, scale, -1, true, options, result, opacity, colors);
            if (truncate && layoutWidth(s.layout.get()) > box.width + .5) {
                pango_layout_set_width(s.layout.get(), int(std::lround(std::max(1., box.width) * PANGO_SCALE)));
                pango_layout_set_ellipsize(s.layout.get(), PANGO_ELLIPSIZE_END);
            }
            const auto w = layoutWidth(s.layout.get());
            addPart(std::move(s), alignment == "center" ? box.x + box.width / 2 - w / 2 : alignment == "right" ? box.x + box.width - w : box.x, y);
        };
        out.natural = layoutWidth(shape(node, value, offsets, 0, value.size(), base, 1, -1, true, options, result, opacity, colors).layout.get());
        if (single && minimumScale < 1 && out.natural > box.width && out.natural > 0) out.scale = std::max(minimumScale, box.width / out.natural);
        spacing(out.scale);
        if (single) { line(0, out.scale, 0); out.count = 1; }
        else {
            auto s = shape(node, value, offsets, 0, value.size(), base, 1, std::max(1., box.width), false, options, result, opacity, colors);
            pango_layout_set_alignment(s.layout.get(), alignment == "center" ? PANGO_ALIGN_CENTER : alignment == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
            auto all = lines(s, spacing(1));
            const auto allowed = std::size_t(maximum);
            if (allowed && all.size() > allowed) {
                const auto cut = all[allowed - 1].begin;
                if (allowed > 1) {
                    all.resize(allowed - 1); out.origins.push_back({box.x, 0}); out.parts.emplace_back(std::move(s), std::move(all));
                }
                line(cut, 1, out.lineHeight * double(allowed - 1)); out.count = allowed;
            } else { out.count = all.size(); out.origins.push_back({box.x, 0}); out.parts.emplace_back(std::move(s), std::move(all)); }
        }
        const auto top = box.y + (box.height - out.lineHeight * double(out.count)) / 2;
        for (auto& origin : out.origins) origin.y += top;
        return out;
    }

    void drawText(cairo_t* cr, const Json& node, const LayerRasterOptions& options, LayerRasterImage& result, double opacity) {
        const auto& descriptor = node["text"]; const auto bounds = rectangle(node["bounds"]);
        const auto value = string(descriptor["string"]);
        if (value.empty() || bounds.width <= 0 || bounds.height <= 0) return;
        if (!Json::validUtf8(value) || utf16Offsets(value).size() > 65537) invalid("Text layout exceeds its UTF-16 limit");
        if (options.plainTextDocument || options.richTextDocument) issue(result, node, "editor document viewports are not implemented");
        Color ink; const bool visible = color(descriptor["foregroundColor"], ink, opacity);
        if (!visible) return;
        if (lineBox(descriptor)) {
            const auto box = lineBoxText(node, value, bounds, options, result, opacity, true); const auto padding = lineBoxPadding(descriptor);
            cairo_save(cr); cairo_new_path(cr);
            cairo_rectangle(cr, bounds.x - padding.x, bounds.y - padding.y, bounds.width + 2 * padding.x, bounds.height + 2 * padding.y); cairo_clip(cr);
            source(cr, ink);
            for (std::size_t i = 0; i < box.parts.size(); ++i) paintLines(cr, box.parts[i].second, box.origins[i].x, box.origins[i].y);
            cairo_restore(cr); return;
        }
        const auto size = number(descriptor["fontSize"], 12); if (size <= 0 || size > 2048) invalid("Text size exceeds its range");
        if (!descriptor["maximumLines"].isNull() || !descriptor["minimumScale"].isNull()) issue(result, node, "maximumLines and minimumScale need a line-box leaf (lineHeight)");
        const auto base = baseFont(node, options, result, size);
        const auto alignment = string(descriptor["alignment"], "left");
        const bool wrapped = flag(descriptor["wrapped"]);
        const auto truncation = string(descriptor["truncation"], "none");
        if (truncation != "none" && truncation != "end") issue(result, node, "unsupported text truncation: " + truncation);
        const auto offsets = utf16Offsets(value);
        const bool bounded = wrapped || truncation == "end" || alignment != "left";
        auto s = shape(node, value, offsets, 0, value.size(), base, 1, bounded ? bounds.width : -1, false, options, result, opacity, true);
        PangoLayout* l = s.layout.get();
        pango_layout_set_alignment(l, alignment == "center" ? PANGO_ALIGN_CENTER : alignment == "right" ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
        pango_layout_set_justify(l, alignment == "justified");
        if (truncation == "end") {
            pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
            if (wrapped) pango_layout_set_height(l, int(std::lround(bounds.height * PANGO_SCALE)));
        } else if (!wrapped && bounded) { pango_layout_set_width(l, -1); } // unwrapped text keeps one line per paragraph
        const double ascent = number(descriptor["font"]["ascender"]), descent = number(descriptor["font"]["descender"]), leading = number(descriptor["font"]["leading"]);
        std::optional<std::pair<double, double>> uniform;
        if (ascent > 0 && ascent - descent + leading > 0) uniform = std::pair{ascent - descent + leading, ascent};
        auto list = lines(s, uniform);
        if (!wrapped && truncation != "end" && alignment != "left") { // alignment within the box for unwrapped lines
            for (auto& line : list) { PangoRectangle logical; pango_layout_line_get_extents(line.line, nullptr, &logical); const double w = double(logical.width) / PANGO_SCALE; line.x = alignment == "center" ? (bounds.width - w) / 2 : alignment == "right" ? bounds.width - w : line.x; }
        }
        result.textDocumentWidth = bounds.width;
        cairo_save(cr); cairo_new_path(cr); cairo_rectangle(cr, bounds.x, bounds.y, bounds.width, bounds.height); cairo_clip(cr);
        source(cr, ink); paintLines(cr, list, bounds.x, bounds.y);
        cairo_restore(cr);
    }

    struct Shadow { Color color; double radius{3}; core::Point offset{0, -3}; const Json* path{}; };
    static std::optional<Shadow> shadow(const Json& node) {
        const auto opacity = std::clamp(number(node["shadowOpacity"]), 0., 1.); if (opacity <= 0) return std::nullopt;
        Shadow s; s.color = {0, 0, 0, 1}; if (!node["shadowColor"].isNull()) color(node["shadowColor"], s.color); s.color.a *= opacity; if (s.color.a <= 0) return std::nullopt;
        s.radius = std::max(0., number(node["shadowRadius"], 3)); s.offset = point(node["shadowOffset"], {0, -3}); if (!node["shadowPath"].isNull()) s.path = &node["shadowPath"];
        return s;
    }

    core::Rect pathBounds(const Json& commands, const std::string& rule, const Matrix& world, const Json* stroke) {
        cairo_t* cr = scratch.get(); cairo_save(cr); cairo_set_matrix(cr, &world); commandPath(cr, commands, rule);
        double x1{}, y1{}, x2{}, y2{}; core::Rect out;
        if (stroke) { strokeStyle(cr, *stroke, number((*stroke)["lineWidth"], 1)); cairo_stroke_extents(cr, &x1, &y1, &x2, &y2); out = transformBounds({x1, y1, x2 - x1, y2 - y1}, world); }
        else { cairo_identity_matrix(cr); cairo_path_extents(cr, &x1, &y1, &x2, &y2); out = {x1, y1, x2 - x1, y2 - y1}; }
        cairo_new_path(cr); cairo_restore(cr); return out;
    }
    core::Rect measure(const Json& node, const Matrix& world, LayerRasterImage& result, std::size_t& visited, unsigned depth) {
        if (++visited > maximumNodes || depth > 64) invalid("Local layer tree exceeds its bound");
        if (!node.isObject()) invalid("Layer is not an object");
        const auto bounds = rectangle(node["bounds"]); auto measured = transformBounds(bounds, world);
        if (string(node["kind"], "layer") == "shape" && !node["shape"]["path"].isNull()) {
            const bool stroked = !node["shape"]["strokeColor"].isNull() && number(node["shape"]["lineWidth"], 1) > 0;
            const auto r = pathBounds(node["shape"]["path"], string(node["shape"]["fillRule"]), world, stroked ? &node["shape"] : nullptr);
            if (r.width >= 0 && r.height >= 0 && (r.width > 0 || r.height > 0)) measured = unite(measured, r);
        }
        if (string(node["kind"], "layer") == "text" && lineBox(node["text"])) {
            const auto p = lineBoxPadding(node["text"]);
            measured = unite(measured, transformBounds({bounds.x - p.x, bounds.y - p.y, bounds.width + 2 * p.x, bounds.height + 2 * p.y}, world));
        }
        if (flag(node["masksToBounds"])) return measured;
        Matrix sub; if (!childTransform(node, sub)) { issue(result, node, "projective child transform must remain in the GPU scene"); return measured; }
        for (auto child : children(node)) {
            if (flag((*child)["hidden"]) || number((*child)["opacity"], 1) <= 0) continue;
            Matrix local; if (!placement(*child, local)) { issue(result, *child, "projective child placement must remain in the GPU scene"); continue; }
            measured = unite(measured, measure(*child, then(then(local, sub), world), result, visited, depth + 1));
        }
        if (const auto s = shadow(node)) {
            auto from = measured; if (s->path) from = pathBounds(*s->path, "", world, nullptr);
            const auto a = apply(world, {0, 0}), b = apply(world, s->offset); const double spread = 3 * shadowSigmaPerRadius * s->radius * areaScale(world);
            measured = unite(measured, {from.x + b.x - a.x - spread, from.y + b.y - a.y - spread, from.width + 2 * spread, from.height + 2 * spread});
        }
        return measured;
    }

    ImagePixels image(const Json& contents, const LayerRasterOptions& options) {
        if (contents.contains("memoryImage")) {
            if (!contents.isObject() || contents.object().size() != 2 || !contents["memoryImage"].isString() || !contents["revision"].isNumber() || !options.memoryImages)
                invalid("Memory image needs its exact key/revision and explicit provider");
            const auto revision = contents["revision"].integer(); if (revision <= 0) invalid("Memory image revision must be positive");
            auto snapshot = options.memoryImages->acquire(contents["memoryImage"].string(), static_cast<std::uint64_t>(revision));
            memoryImageUsed = true;
            if (!snapshot) { static constexpr std::array<std::uint8_t, 4> clear{}; ++counts.missingMemoryImages; memoryImageMissing = true; return {1, 1, clear, {}}; }
            return {snapshot->width(), snapshot->height(), snapshot->premultipliedBGRA(), std::move(snapshot)};
        }
        const auto expected = string(contents["sha256"]), relative = string(contents["asset"]);
        if (expected.size() != 64 || expected.find_first_not_of("0123456789abcdef") != std::string::npos) invalid("Intrinsic image requires SHA-256");
        const std::filesystem::path path(relative);
        if (options.assetRoot.empty() || path.is_absolute() || relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos) invalid("Image needs a confined explicit fixture path");
        for (const auto& part : path) if (part == ".." || part == ".") invalid("Image path traversal rejected");
        if (const auto it = images.find(expected); it != images.end()) return {it->second.width, it->second.height, it->second.premultipliedBGRA, {}};
        if (images.size() >= 256) invalid("Decoded intrinsic image cache is full; clear unused content");
        const auto root = std::filesystem::canonical(options.assetRoot), file = std::filesystem::canonical(root / path);
        auto r = root.begin(), f = file.begin(); for (; r != root.end(); ++r, ++f) if (f == file.end() || *r != *f) invalid("Image path escapes fixture root");
        const auto size = std::filesystem::file_size(file); if (size == 0 || size > 64 * 1024 * 1024) invalid("Intrinsic image exceeds its file bound");
        std::ifstream input(file, std::ios::binary); std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size), 0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Cannot read explicit intrinsic image");
        if (sha256(bytes) != expected) invalid("Intrinsic image hash mismatch");
        std::unique_ptr<GdkPixbufLoader, ObjectDeleter> loader(gdk_pixbuf_loader_new());
        const bool wrote = gdk_pixbuf_loader_write(loader.get(), bytes.data(), bytes.size(), nullptr);
        const bool closed = gdk_pixbuf_loader_close(loader.get(), nullptr);
        GdkPixbuf* pixbuf = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader.get()) : nullptr;
        if (!pixbuf || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8) invalid("Cannot decode intrinsic image");
        DecodedImage decoded; decoded.width = unsigned(gdk_pixbuf_get_width(pixbuf)); decoded.height = unsigned(gdk_pixbuf_get_height(pixbuf));
        if (!decoded.width || !decoded.height || std::size_t(decoded.width) * decoded.height > maximumPixels) invalid("Intrinsic image exceeds its pixel bound");
        const auto count = std::size_t(decoded.width) * decoded.height * 4;
        if (count > maximumResourceBytes - heldBytes()) invalid("Layer resource budget exceeded");
        const int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
        const guchar* rows = gdk_pixbuf_read_pixels(pixbuf);
        decoded.premultipliedBGRA.resize(count);
        for (unsigned y = 0; y < decoded.height; ++y) for (unsigned x = 0; x < decoded.width; ++x) {
            const guchar* p = rows + std::size_t(y) * stride + std::size_t(x) * channels;
            const unsigned a = channels == 4 ? p[3] : 255;
            auto* out = &decoded.premultipliedBGRA[(std::size_t(y) * decoded.width + x) * 4];
            out[0] = std::uint8_t((p[2] * a + 127) / 255); out[1] = std::uint8_t((p[1] * a + 127) / 255); out[2] = std::uint8_t((p[0] * a + 127) / 255); out[3] = std::uint8_t(a);
        }
        counts.resourceBytes += count; ++counts.imageDecodes; const auto& stored = images.emplace(expected, std::move(decoded)).first->second;
        return {stored.width, stored.height, stored.premultipliedBGRA, {}};
    }

    void drawShadow(cairo_t* cr, const Json& node, const Matrix& world, const Matrix& pixels, const LayerRasterOptions& options, LayerRasterImage& result, double opacity, unsigned depth) {
        const auto s = shadow(node); if (!s) return;
        const auto device = then(world, pixels);
        const double sigma = shadowSigmaPerRadius * s->radius * areaScale(device);
        const auto a = apply(device, {0, 0}), b = apply(device, s->offset); const core::Point shift{b.x - a.x, b.y - a.y};
        // Only the area the casting shape and its blur can reach is allocated and filtered.
        LayerRasterImage extent; std::size_t measured{};
        const auto cast = transformBounds(s->path ? pathBounds(*s->path, "", world, nullptr) : measure(node, world, extent, measured, depth), pixels);
        const double reach = std::ceil(3 * sigma) + 2, marginX = std::abs(shift.x) + reach, marginY = std::abs(shift.y) + reach;
        const double left = std::floor(std::max(cast.x - reach, -marginX)), top = std::floor(std::max(cast.y - reach, -marginY));
        const double right = std::ceil(std::min(cast.x + cast.width + reach, result.width + marginX)), bottom = std::ceil(std::min(cast.y + cast.height + reach, result.height + marginY));
        if (right <= left || bottom <= top) return;
        const unsigned w = unsigned(right - left), h = unsigned(bottom - top);
        if (std::size_t(w) * h > 4 * maximumPixels) invalid("Layer shadow exceeds its pixel limit");
        const auto local = then(pixels, translation(-left, -top)), transform = then(world, local);
        Surface shape = newSurface(w, h); Context ink = newContext(shape.get());
        if (s->path) { cairo_set_matrix(ink.get(), &transform); commandPath(ink.get(), *s->path, ""); cairo_set_source_rgba(ink.get(), 1, 1, 1, 1); cairo_fill(ink.get()); }
        else { std::size_t visited{}; draw(ink.get(), node, world, local, options, result, 1, visited, depth, true, true); }
        cairo_surface_flush(shape.get());
        const auto* data = cairo_image_surface_get_data(shape.get()); const int stride = cairo_image_surface_get_stride(shape.get());
        std::vector<float> alpha(std::size_t(w) * h);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) alpha[std::size_t(y) * w + x] = data[std::size_t(y) * stride + x * 4 + 3] / 255.f;
        blurAlpha(alpha, w, h, sigma);
        Surface blurred = newSurface(w, h); cairo_surface_flush(blurred.get());
        auto* out = cairo_image_surface_get_data(blurred.get()); const int outStride = cairo_image_surface_get_stride(blurred.get());
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            const double a = std::clamp(alpha[std::size_t(y) * w + x] * s->color.a * opacity, 0., 1.);
            auto* p = out + std::size_t(y) * outStride + x * 4;
            p[0] = std::uint8_t(std::lround(s->color.b * a * 255)); p[1] = std::uint8_t(std::lround(s->color.g * a * 255)); p[2] = std::uint8_t(std::lround(s->color.r * a * 255)); p[3] = std::uint8_t(std::lround(a * 255));
        }
        cairo_surface_mark_dirty(blurred.get());
        cairo_save(cr); cairo_identity_matrix(cr); cairo_set_source_surface(cr, blurred.get(), left + shift.x, top + shift.y); cairo_paint(cr); cairo_restore(cr);
    }

    void draw(cairo_t* cr, const Json& node, const Matrix& world, const Matrix& pixels, const LayerRasterOptions& options, LayerRasterImage& result,
              double inheritedOpacity, std::size_t& visited, unsigned depth, bool root, bool shadowSource = false) {
        if (++visited > maximumNodes || depth > 64) invalid("Local layer tree exceeds its bound");
        if (flag(node["hidden"])) return;
        const double ownOpacity = (root && !options.includeRootOpacity) || shadowSource ? 1. : std::clamp(number(node["opacity"], 1), 0., 1.);
        if (ownOpacity <= 0 || inheritedOpacity <= 0) return;
        ++counts.nodesDrawn; const auto bounds = rectangle(node["bounds"]); const auto matrix = then(world, pixels);
        if (std::abs(matrix.xx * matrix.yy - matrix.xy * matrix.yx) < 1e-12) return; // a collapsed plane draws nothing
        cairo_save(cr); cairo_set_matrix(cr, &matrix);
        struct Pushed { bool group; double opacity; }; std::vector<Pushed> pushed;
        const auto push = [&](bool group, double opacity) {
            cairo_save(cr); if (group) cairo_push_group(cr); pushed.push_back({group, opacity});
        };
        double paintOpacity = inheritedOpacity;
        if (flag(node["allowsGroupOpacity"], true)) { if (ownOpacity != 1 || inheritedOpacity != 1) push(true, ownOpacity * inheritedOpacity); paintOpacity = 1; }
        else paintOpacity *= ownOpacity;
        if (flag(node["masksToBounds"]) && !shadowSource) { push(false, 1); boundsPath(cr, node); cairo_clip(cr); }
        if ((!root || options.includeRootMask) && !shadowSource && !node["mask"].isNull()) {
            const auto& mask = node["mask"]; Matrix transform;
            if (string(mask["kind"]) == "shape" && placement(mask, transform) && mask["shape"]["strokeColor"].isNull() && (mask["children"].isNull() || array(mask["children"], maximumNodes).empty())) {
                Color fill; const double alpha = color(mask["shape"]["fillColor"], fill) ? fill.a * std::clamp(number(mask["opacity"], 1), 0., 1.) : 0;
                const double opacity = flag(mask["hidden"]) ? 0 : alpha;
                cairo_save(cr);
                const auto masked = then(transform, matrix);
                cairo_set_matrix(cr, &masked); commandPath(cr, mask["shape"]["path"], string(mask["shape"]["fillRule"]));
                cairo_clip(cr); cairo_set_matrix(cr, &matrix); cairo_push_group(cr); pushed.push_back({true, opacity});
            } else issue(result, node, "only local filled-shape alpha masks are implemented");
        }
        if (!shadowSource) drawShadow(cr, node, world, pixels, options, result, paintOpacity, depth);
        if (flag(node["geometryFlipped"])) issue(result, node, "flipped local layer geometry is not implemented; preserve it in the scene adapter");
        if (node.contains("contentsAreFlipped") && !flag(node["contentsAreFlipped"])) issue(result, node, "unflipped Core Animation content convention is not implemented");
        if (string(node["cornerCurve"], "circular") != "circular") issue(result, node, "continuous corner curve is not implemented");
        if (number(node["cornerRadius"]) > 0 && number(node["maskedCorners"], 15) != 15) issue(result, node, "selective rounded corners are not implemented");
        const auto className = string(node["class"], "CALayer");
        if (className != "CALayer" && className != "CAShapeLayer" && className != "CATextLayer" && className != "CAGradientLayer" && className != "HUDStandardTextLayer") issue(result, node, "custom layer behavior is not implemented: " + className);
        if (!node["animationKeys"].isNull() && !array(node["animationKeys"], 1024).empty()) issue(result, node, "model state only; presentation animation is owned by the scene");
        Color background; if (color(node["backgroundColor"], background, paintOpacity)) { boundsPath(cr, node); cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING); source(cr, background); cairo_fill(cr); }
        const auto kind = string(node["kind"], "layer");
        if (kind == "shape") {
            const auto& shapeNode = node["shape"]; const auto rule = string(shapeNode["fillRule"]);
            Color fill; if (color(shapeNode["fillColor"], fill, paintOpacity)) { commandPath(cr, shapeNode["path"], rule); source(cr, fill); cairo_fill(cr); }
            Color outline; const bool stroked = color(shapeNode["strokeColor"], outline, paintOpacity); const auto width = number(shapeNode["lineWidth"], 1);
            if (number(shapeNode["strokeStart"]) != 0 || number(shapeNode["strokeEnd"], 1) != 1) issue(result, node, "partial stroke ranges are not implemented");
            else if (stroked && width > 0) { cairo_save(cr); commandPath(cr, shapeNode["path"], rule); strokeStyle(cr, shapeNode, width); source(cr, outline); cairo_stroke(cr); cairo_restore(cr); }
        } else if (kind == "text") drawText(cr, node, options, result, paintOpacity);
        else if (kind == "gradient") {
            const auto& g = node["gradient"]; const auto type = string(g["type"], "axial");
            if (type != "axial" && type != "radial") issue(result, node, "only axial and radial native gradients are implemented");
            else {
                const auto& colors = array(g["colors"], 256);
                const bool hasLocations = !g["locations"].isNull(); if (hasLocations && array(g["locations"], 256).size() != colors.size()) invalid("Gradient stop count mismatch");
                const auto a = point(g["startPoint"], {.5, 0}), b = point(g["endPoint"], {.5, 1});
                const core::Point start{bounds.x + a.x * bounds.width, bounds.y + a.y * bounds.height};
                cairo_pattern_t* pattern = nullptr;
                if (type == "axial") pattern = cairo_pattern_create_linear(start.x, start.y, bounds.x + b.x * bounds.width, bounds.y + b.y * bounds.height);
                else {
                    const double rx = std::abs(b.x - a.x) * bounds.width, ry = std::abs(b.y - a.y) * bounds.height;
                    if (rx > 0 && ry > 0) {
                        pattern = cairo_pattern_create_radial(0, 0, 0, 0, 0, 1);
                        Matrix m; cairo_matrix_init_scale(&m, 1 / rx, 1 / ry); cairo_matrix_translate(&m, -start.x, -start.y); cairo_pattern_set_matrix(pattern, &m);
                    } else issue(result, node, "degenerate radial gradient (startPoint and endPoint share an axis)");
                }
                if (pattern) {
                    double previous = -1;
                    for (std::size_t i = 0; i < colors.size(); ++i) {
                        Color c; color(colors[i], c, paintOpacity);
                        const double at = hasLocations ? number(g["locations"].array()[i]) : colors.size() > 1 ? double(i) / double(colors.size() - 1) : 0.;
                        if (at < previous || at < 0 || at > 1) { cairo_pattern_destroy(pattern); invalid("Invalid gradient stop location"); }
                        previous = at; cairo_pattern_add_color_stop_rgba(pattern, at, c.r, c.g, c.b, c.a);
                    }
                    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
                    if (!colors.empty()) { boundsPath(cr, node); cairo_set_source(cr, pattern); cairo_fill(cr); }
                    cairo_pattern_destroy(pattern);
                }
            }
        } else if (kind != "layer") issue(result, node, "unknown native layer kind: " + kind);
        if (!node["contents"].isNull()) drawContents(cr, node, options, result, paintOpacity, bounds);
        Matrix sub;
        if (!childTransform(node, sub)) issue(result, node, "projective child transform must remain in the GPU scene");
        else for (auto child : children(node)) {
            if (flag((*child)["hidden"]) || number((*child)["opacity"], 1) <= 0) continue;
            Matrix local; if (!placement(*child, local)) { issue(result, *child, "projective child placement must remain in the GPU scene"); continue; }
            draw(cr, *child, then(then(local, sub), world), pixels, options, result, paintOpacity, visited, depth + 1, false);
        }
        cairo_set_matrix(cr, &matrix);
        const auto borderWidth = number(node["borderWidth"]); Color border;
        if (color(node["borderColor"], border, paintOpacity) && borderWidth > 0) {
            // Layer borders lie inside the bounds, so the stroked path is inset.
            const double inset = std::min(borderWidth / 2, std::min(bounds.width, bounds.height) / 2);
            cairo_save(cr); cairo_new_path(cr);
            roundedRect(cr, {bounds.x + inset, bounds.y + inset, bounds.width - 2 * inset, bounds.height - 2 * inset}, std::max(0., number(node["cornerRadius"]) - inset));
            cairo_set_dash(cr, nullptr, 0, 0); cairo_set_line_width(cr, inset * 2); source(cr, border); cairo_stroke(cr); cairo_restore(cr);
        }
        while (!pushed.empty()) {
            const auto p = pushed.back(); pushed.pop_back();
            if (p.group) { cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, p.opacity); }
            cairo_restore(cr);
        }
        cairo_restore(cr);
    }

    void drawContents(cairo_t* cr, const Json& node, const LayerRasterOptions& options, LayerRasterImage& result, double opacity, core::Rect bounds) {
        const auto decoded = image(node["contents"], options);
        std::vector<std::uint8_t> pixels(decoded.premultipliedBGRA.begin(), decoded.premultipliedBGRA.end());
        if (!node["contentsSourceInTint"].isNull()) {
            Color tint; color(node["contentsSourceInTint"], tint);
            const std::array<unsigned, 4> ink{unsigned(std::lround(tint.b * tint.a * 255)), unsigned(std::lround(tint.g * tint.a * 255)), unsigned(std::lround(tint.r * tint.a * 255)), unsigned(std::lround(tint.a * 255))};
            for (std::size_t at = 0; at < pixels.size(); at += 4) { const unsigned alpha = pixels[at + 3]; for (unsigned c = 0; c < 4; ++c) pixels[at + c] = std::uint8_t((ink[c] * alpha + 127u) / 255u); }
        }
        Surface bitmap(cairo_image_surface_create_for_data(pixels.data(), CAIRO_FORMAT_ARGB32, int(decoded.width), int(decoded.height), int(decoded.width * 4)));
        auto destination = bounds; const auto gravity = string(node["contentsGravity"], "resize");
        const auto crop = node["contentsRect"].isNull() ? core::Rect{0, 0, 1, 1} : rectangle(node["contentsRect"]);
        const auto center = node["contentsCenter"].isNull() ? core::Rect{0, 0, 1, 1} : rectangle(node["contentsCenter"]);
        if (center.x != 0 || center.y != 0 || center.width != 1 || center.height != 1) issue(result, node, "nine-slice native contents are not implemented");
        const double sw = decoded.width * crop.width, sh = decoded.height * crop.height;
        if (gravity == "resizeAspect" || gravity == "resizeAspectFill") {
            const double sx = bounds.width / std::max(1., sw), sy = bounds.height / std::max(1., sh), scale = gravity == "resizeAspect" ? std::min(sx, sy) : std::max(sx, sy);
            destination = {bounds.x + (bounds.width - sw * scale) / 2, bounds.y + (bounds.height - sh * scale) / 2, sw * scale, sh * scale};
        } else if (gravity != "resize") issue(result, node, "unsupported intrinsic image gravity: " + gravity);
        if (destination.width <= 0 || destination.height <= 0 || sw <= 0 || sh <= 0) return;
        cairo_save(cr); cairo_new_path(cr); cairo_rectangle(cr, destination.x, destination.y, destination.width, destination.height); cairo_clip(cr);
        cairo_translate(cr, destination.x, destination.y); cairo_scale(cr, destination.width / sw, destination.height / sh);
        cairo_set_source_surface(cr, bitmap.get(), -double(decoded.width) * crop.x, -double(decoded.height) * crop.y);
        cairo_pattern_set_filter(cairo_get_source(cr), string(node["magnificationFilter"], "linear") == "nearest" ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
        cairo_paint_with_alpha(cr, opacity);
        cairo_restore(cr);
        cairo_surface_flush(bitmap.get());
        cairo_surface_finish(bitmap.get());
    }

    void measureRaster(const Json& layer, const LayerRasterOptions& options, LayerRasterImage& result) {
        std::size_t visited = 0; auto bounds = measure(layer, identityMatrix(), result, visited, 0);
        bounds.x -= options.paddingPoints; bounds.y -= options.paddingPoints; bounds.width += options.paddingPoints * 2; bounds.height += options.paddingPoints * 2;
        const double density = rasterDensity(layer, options);
        const double w = std::ceil(std::max(1.0 / density, bounds.width) * density), h = std::ceil(std::max(1.0 / density, bounds.height) * density);
        if (w > 8192 || h > 8192 || w * h > maximumPixels) invalid("Local raster exceeds its pixel limit");
        result.width = unsigned(w); result.height = unsigned(h); bounds.width = w / density; bounds.height = h / density; result.bounds = bounds;
    }
    // Paints a measured raster and converts it to tight straight RGBA rows.
    void paintRaster(const Json& layer, const LayerRasterOptions& options, LayerRasterImage& result, std::vector<std::uint8_t>& out) {
        Surface surface = newSurface(result.width, result.height);
        {
            Context cr = newContext(surface.get());
            const double density = rasterDensity(layer, options);
            Matrix pixels = translation(-result.bounds.x, -result.bounds.y); Matrix scale; cairo_matrix_init_scale(&scale, density, density);
            pixels = then(pixels, scale);
            std::size_t visited = 0; draw(cr.get(), layer, identityMatrix(), pixels, options, result, 1, visited, 0, true);
            if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) throw std::runtime_error(std::string("Complete local layer raster: ") + cairo_status_to_string(cairo_status(cr.get())));
        }
        cairo_surface_flush(surface.get());
        const auto* data = cairo_image_surface_get_data(surface.get()); const int stride = cairo_image_surface_get_stride(surface.get());
        out.resize(std::size_t(result.width) * result.height * 4);
        for (unsigned y = 0; y < result.height; ++y) for (unsigned x = 0; x < result.width; ++x) {
            std::uint32_t value; std::memcpy(&value, data + std::size_t(y) * stride + x * 4, 4);
            const unsigned a = value >> 24, r = (value >> 16) & 255, g = (value >> 8) & 255, b = value & 255;
            auto* p = &out[(std::size_t(y) * result.width + x) * 4];
            p[0] = a ? std::uint8_t(std::min(255u, (r * 255u + a / 2) / a)) : 0;
            p[1] = a ? std::uint8_t(std::min(255u, (g * 255u + a / 2) / a)) : 0;
            p[2] = a ? std::uint8_t(std::min(255u, (b * 255u + a / 2) / a)) : 0;
            p[3] = std::uint8_t(a);
        }
    }
    void repaint(const LayerRasterImage& image, std::vector<std::uint8_t>& out) {
        if (!image.recipe_) invalid("Only a releasable raster can be painted again");
        const auto& recipe = *image.recipe_;
        struct Restore {
            Impl& r; LayerFontLanguage language; LayerRasterStats counts; bool memory;
            ~Restore() { const auto decoded = r.counts.resourceBytes; r.language = language; r.counts = counts; r.counts.resourceBytes = decoded; r.memoryImageUsed = memory; }
        } restore{*this, language, counts, memoryImageUsed};
        language = recipe.language;
        LayerRasterImage scratchImage; measureRaster(*recipe.layer, recipe.options, scratchImage);
        if (scratchImage.width != image.width || scratchImage.height != image.height || scratchImage.bounds != image.bounds) throw std::logic_error("A released raster no longer paints its retained bounds");
        paintRaster(*recipe.layer, recipe.options, scratchImage, out);
        restore.counts.pixelRepaints++;
    }

    std::shared_ptr<const LayerRasterImage> raster(std::string id, std::uint64_t revision, const Json& layer, const LayerRasterOptions& options, bool releasable, std::shared_ptr<const Recipe> reuse) {
        onThread();
        if (id.empty() || id.size() > 4096 || !Json::validUtf8(id)) invalid("Invalid retained layer ID");
        if (!std::isfinite(options.pixelsPerPoint) || options.pixelsPerPoint < .25 || options.pixelsPerPoint > core::maximumRenderScale * core::renderMagnification(options.magnification)
            || !std::isfinite(options.paddingPoints) || options.paddingPoints < 0 || options.paddingPoints > 64) invalid("Invalid local raster scale/padding");
        if (options.retainedPlainText) invalid("Retained editor text layouts are not implemented");
        const auto old = entries.find(id);
        if (old != entries.end() && old->second.revision == revision && old->second.options == options && (!old->second.hasText || old->second.fontRevision == fontRevision)) {
            auto& image = *old->second.image;
            if (image.pixelsReleased()) {
                if (image.pixelBytes() > maximumResourceBytes - heldBytes()) invalid("Retained layer resource budget exceeded");
                repaint(image, image.straightRGBA); releasedBytes -= image.pixelBytes();
            }
            ++counts.cacheHits; return old->second.image;
        }
        if (old == entries.end() && entries.size() >= maximumEntries) invalid("Retained layer cache is full; remove unused source IDs");
        auto result = std::make_shared<LayerRasterImage>();
        measureRaster(layer, options, *result);
        const auto bytes = result->pixelBytes(), previous = old == entries.end() ? 0 : old->second.image->pixelBytes();
        const auto previousReleased = old != entries.end() && old->second.image->pixelsReleased() ? previous : 0, previousHeld = previous - previousReleased;
        if (bytes > maximumResourceBytes - (heldBytes() - previousHeld)) invalid("Retained layer resource budget exceeded");
        memoryImageUsed = memoryImageMissing = false;
        std::vector<std::uint8_t> pixelsOut; paintRaster(layer, options, *result, pixelsOut);
        const bool memoryImage = memoryImageUsed, placeholder = memoryImageMissing; memoryImageUsed = memoryImageMissing = false;
        if (bytes > maximumResourceBytes - (heldBytes() - previousHeld)) invalid("Retained layer resource budget exceeded after image decode");
        result->straightRGBA = std::move(pixelsOut);
        if (placeholder) { ++counts.rasterizations; return result; }
        const bool hasText = containsText(layer); const bool document = options.plainTextDocument || options.richTextDocument;
        const auto oldDescriptors = old == entries.end() ? 0 : old->second.descriptorBytes;
        std::shared_ptr<const Recipe> sourceRecipe; std::size_t descriptorBytes{};
        releasable = releasable && !document && !memoryImage;
        if (hasText && !document) {
            const bool same = reuse && reuse->layer.get() == &layer;
            if (same) descriptorBytes = reuse->bytes;
            else if (!jsonBytesWithin(layer, maximumTypographyDescriptorBytes - (counts.typographyDescriptorBytes - oldDescriptors), descriptorBytes)) invalid("Retained typography descriptor budget exceeded");
            sourceRecipe = std::make_shared<const Recipe>(Recipe{same ? reuse->layer : std::make_shared<const Json>(layer), options, language, descriptorBytes});
        } else if (releasable) {
            std::size_t size{};
            if (jsonBytesWithin(layer, bytes, size)) sourceRecipe = std::make_shared<const Recipe>(Recipe{std::make_shared<const Json>(layer), options, language, size});
        }
        if (releasable && sourceRecipe) result->recipe_ = sourceRecipe; else releasable = false;
        std::string owner(LayerRasterAttribution::current()); if (owner.empty() && old != entries.end()) owner = std::move(old->second.owner);
        entries.insert_or_assign(std::move(id), Entry{revision, options, result, fontRevision, hasText, hasText && !document ? std::move(sourceRecipe) : nullptr, descriptorBytes, std::move(owner), releasable});
        counts.typographyDescriptorBytes = counts.typographyDescriptorBytes - oldDescriptors + descriptorBytes;
        counts.resourceBytes = counts.resourceBytes - previous + bytes; releasedBytes -= previousReleased; ++counts.rasterizations; return result;
    }
};

LayerPlainTextAnalysis::LayerPlainTextAnalysis(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LayerPlainTextAnalysis::~LayerPlainTextAnalysis() = default;
const LayerPlainTextMetrics& LayerPlainTextAnalysis::metrics() const noexcept { return impl_->metrics; }
std::uint64_t LayerPlainTextAnalysis::layoutsCreated() const noexcept { return impl_->created; }

namespace {
std::uint32_t utf16Length(std::string_view text) {
    std::uint32_t n = 0;
    for (const char c : text) { const auto b = static_cast<unsigned char>(c); if ((b & 0xc0) != 0x80) n += b >= 0xf0 ? 2 : 1; }
    return n;
}
// Byte offset of each line start, ending with the text length.
std::vector<std::size_t> lineStarts(PangoLayout* layout, std::size_t size) {
    std::vector<std::size_t> starts;
    for (int i = 0; i < pango_layout_get_line_count(layout); ++i) starts.push_back(std::size_t(pango_layout_line_get_start_index(pango_layout_get_line_readonly(layout, i))));
    starts.push_back(size); return starts;
}
} // namespace

std::span<const std::uint32_t> LayerPlainTextAnalysis::lineLengths(std::u16string_view value, double width, std::size_t maximumLines) {
    auto& r = *impl_; r.onThread();
    if (!std::isfinite(width) || std::abs(width) > 32768 || maximumLines == 0) invalid("Plain text analysis exceeds explicit bounds");
    const auto text = fromUtf16(value);
    const auto layout = r.shape(text, std::max(1., width), {}); ++r.created;
    const auto starts = lineStarts(layout.get(), text.size());
    if (starts.size() - 1 > maximumLines) invalid("Plain text line index exceeds its explicit budget");
    r.lengths.clear();
    for (std::size_t i = 0; i + 1 < starts.size(); ++i) r.lengths.push_back(utf16Length(std::string_view(text).substr(starts[i], starts[i + 1] - starts[i])));
    return r.lengths;
}
std::span<const LayerStyledTextLine> LayerPlainTextAnalysis::richLines(std::u16string_view value, double width, std::span<const core::notes::TextRun> runs, std::size_t maximumLines) {
    auto& r = *impl_; r.onThread();
    if (!std::isfinite(width) || std::abs(width) > 32768 || maximumLines == 0 || runs.size() > core::notes::RichText::maximumRuns) invalid("Rich text analysis exceeds explicit bounds");
    std::uint64_t previous{};
    for (const auto& run : runs) { const std::uint64_t end = std::uint64_t(run.location) + run.length; if (!run.style.valid() || run.length == 0 || run.location < previous || end > value.size()) invalid("Invalid rich analysis run"); previous = end; }
    const auto text = fromUtf16(value);
    const auto layout = r.shape(text, std::max(1., width), runs); ++r.created;
    const auto starts = lineStarts(layout.get(), text.size());
    if (starts.size() - 1 > maximumLines) invalid("Rich text line index exceeds its budget");
    r.styledLines.clear();
    for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
        PangoRectangle logical; pango_layout_line_get_extents(pango_layout_get_line_readonly(layout.get(), int(i)), nullptr, &logical);
        const auto length = utf16Length(std::string_view(text).substr(starts[i], starts[i + 1] - starts[i]));
        r.styledLines.push_back({length, length ? std::max(1., std::ceil(double(logical.height) / PANGO_SCALE) + 1) : r.metrics.lineHeight});
    }
    return r.styledLines;
}

LayerRasterizer::LayerRasterizer() : LayerRasterizer(std::filesystem::path{}) {}
LayerRasterizer::LayerRasterizer(const std::filesystem::path& fontDirectory) : impl_(std::make_unique<Impl>(fontDirectory)) {}
LayerRasterizer::~LayerRasterizer() = default;
bool LayerRasterizer::setDefaultFontLanguage(LayerFontLanguage language) {
    auto& r = *impl_; r.onThread();
    if (std::find(core::textRegions.begin(), core::textRegions.end(), language) == core::textRegions.end()) invalid("Invalid default font language");
    if (r.language == language) return false;
    r.language = language; ++r.fontRevision; return true;
}
std::span<const std::string> LayerRasterizer::bundledFontFamilies() const { impl_->onThread(); return impl_->bundledNames; }
std::optional<std::string> LayerRasterizer::fontFamilyOf(std::string_view name) const {
    impl_->onThread(); if (auto face = impl_->bundledFace(name)) return face;
    if (const auto mapped = core::notes::notesWindowsFontFamily(name)) return std::string(*mapped);
    return std::nullopt;
}
std::string LayerRasterizer::regionFontFamily(LayerFontLanguage language) const { impl_->onThread(); return impl_->regionFamily(language); }
LayerFontLanguage LayerRasterizer::fontLanguage() const { impl_->onThread(); return impl_->language; }
std::uint64_t LayerRasterizer::fontRevision() const { impl_->onThread(); return impl_->fontRevision; }
std::string_view LayerRasterizer::defaultFontFamily() const { impl_->onThread(); return impl_->latinFamily.empty() ? std::string_view("sans-serif") : std::string_view(impl_->latinFamily); }
const std::vector<std::string>& LayerRasterizer::installedFontFamilies() {
    auto& r = *impl_; r.onThread();
    if (!r.installedFontNames) r.installedFontNames = std::vector<std::string>(r.available.begin(), r.available.end());
    return *r.installedFontNames;
}
std::unique_ptr<LayerPlainTextAnalysis> LayerRasterizer::plainSystemTextAnalysis(const std::string& id, double size, const LayerRasterOptions& options) {
    auto& r = *impl_; r.onThread();
    if (id.empty() || id.size() > 4096 || !Json::validUtf8(id) || !std::isfinite(size) || size <= 0 || size > 2048) invalid("Invalid plain system font request");
    const Json font = Json::Object{{"familyName", ".AppleSystemUIFont"}, {"postScriptName", ".SFNS-Regular"}};
    const Json node = Json::Object{{"id", id}}; LayerRasterImage report;
    const auto family = r.fontFamily(font, node, options, report);
    const auto natural = r.lineMetrics(family, PANGO_WEIGHT_NORMAL, false);
    auto analysis = std::make_unique<LayerPlainTextAnalysis::Impl>();
    auto& m = analysis->metrics;
    m.fontSize = size; m.ascent = natural[0] * size; m.descent = natural[1] * size; m.leading = std::max(0., natural[2] * size);
    m.lineHeight = std::ceil(m.ascent + m.descent + m.leading) + 1;
    m.selectedFamily = report.fontSubstitutions.empty() ? std::string(".AppleSystemUIFont") : r.fallbackFamily(options);
    m.fontSubstitutions = report.fontSubstitutions;
    auto* owner = &r;
    analysis->shape = [owner, id, options, family, size](const std::string& text, double width, std::span<const core::notes::TextRun> runs) {
        Json::Array list;
        for (const auto& run : runs) list.push_back(Json::Object{{"utf16Range", Json::Array{double(run.location), double(run.length)}},
            {"attributes", Json::Object{{"NSFont", notesRunFont(run.style)}}}});
        Json::Object descriptor{{"string", text}};
        if (!list.empty()) descriptor.emplace("runs", Json(std::move(list)));
        const Json node = Json::Object{{"id", id}, {"text", Json(std::move(descriptor))}};
        LayerRasterImage report;
        FontSpec base{family, PANGO_WEIGHT_NORMAL, false, size};
        auto shaped = owner->shape(node, text, utf16Offsets(text), 0, text.size(), base, 1, width, false, options, report, 1, false);
        return std::move(shaped.layout);
    };
    ++r.counts.textAnalysisFormatsCreated;
    return std::unique_ptr<LayerPlainTextAnalysis>(new LayerPlainTextAnalysis(std::move(analysis)));
}
LayerSourceTextMeasurement LayerRasterizer::measureSourceText(const std::string& id, const Json& descriptor, double width, const LayerRasterOptions& options) {
    auto& r = *impl_; r.onThread(); const bool box = lineBox(descriptor);
    if (id.empty() || id.size() > 4096 || !Json::validUtf8(id) || !descriptor.isObject() || !descriptor["string"].isString() ||
        (!box && !descriptor["wrapped"].isBool()) || !std::isfinite(width) || width <= 0 || width > 3e9) invalid("Invalid source caption measurement");
    if (!box && !descriptor["runs"].isNull() && (!descriptor["runs"].isArray() || !descriptor["runs"].array().empty())) invalid("Caption measurement does not flatten attributed runs");
    const std::string value(descriptor["string"].string()); if (value.size() > 262144) invalid("Caption measurement exceeds painted UTF-16 bound");
    const auto size = number(descriptor["fontSize"], box ? 12. : 0.); if (size <= 0 || size > 2048) invalid("Invalid caption measurement font size");
    const Json node = Json::Object{{"id", id}, {"text", descriptor}}; LayerRasterImage report;
    ++r.counts.textAnalysisFormatsCreated; LayerSourceTextMeasurement result;
    if (box && !value.empty()) {
        const auto text = r.lineBoxText(node, value, {0, 0, width, 0}, options, report, 1, false);
        result.width = std::ceil(text.natural * 2) / 2; result.height = text.lineHeight * double(text.count); result.scale = text.scale; result.lines = text.count;
    }
    const auto base = r.baseFont(node, options, report, size);
    const auto strut = r.lineMetrics(base.font, base.weight, base.italic);
    result.ascent = strut[0] * size * result.scale; result.descent = strut[1] * size * result.scale;
    if (box) { result.fontSubstitutions = std::move(report.fontSubstitutions); return result; }
    const bool wrapped = flag(descriptor["wrapped"]);
    result.fontSubstitutions = report.fontSubstitutions;
    if (value.empty()) return result;
    const auto shaped = r.shape(node, value, utf16Offsets(value), 0, value.size(), base, 1, wrapped ? width : -1, false, options, report, 1, false);
    const double ascent = number(descriptor["font"]["ascender"]), descent = number(descriptor["font"]["descender"]), leading = number(descriptor["font"]["leading"]);
    std::optional<std::pair<double, double>> uniform;
    if (ascent > 0 && ascent - descent + leading > 0) uniform = std::pair{ascent - descent + leading, ascent};
    const auto list = r.lines(shaped, uniform);
    result.width = LayerRasterizer::Impl::layoutWidth(shaped.layout.get()); result.lines = list.size();
    for (const auto& line : list) result.height += line.height;
    return result;
}
std::shared_ptr<const LayerRasterImage> LayerRasterizer::rasterize(std::string id, std::uint64_t revision, const Json& layer, const LayerRasterOptions& options) {
    return impl_->raster(std::move(id), revision, layer, options, false, {});
}
std::shared_ptr<const LayerRasterImage> LayerRasterizer::rasterize(std::string id, std::uint64_t revision, const Json& layer, const LayerRasterOptions& options, LayerRasterRetention retention) {
    if (retention != LayerRasterRetention::pixels && retention != LayerRasterRetention::releasable) invalid("Unknown raster retention");
    return impl_->raster(std::move(id), revision, layer, options, retention == LayerRasterRetention::releasable, {});
}
bool LayerRasterizer::releasePixels(const std::string& id, const std::shared_ptr<const LayerRasterImage>& image) {
    auto& r = *impl_; r.onThread();
    if (!image || !image->recipe_ || image->straightRGBA.empty()) return false;
    const auto found = r.entries.find(id); const bool cached = found != r.entries.end() && found->second.image.get() == image.get();
    if (image.use_count() != (cached ? 2 : 1)) return false;
    std::vector<std::uint8_t>().swap(const_cast<LayerRasterImage&>(*image).straightRGBA);
    if (cached) r.releasedBytes += image->pixelBytes();
    ++r.counts.pixelReleases; return true;
}
void LayerRasterizer::repaint(const LayerRasterImage& image, std::vector<std::uint8_t>& straightRGBA) {
    auto& r = *impl_; r.onThread();
    if (&straightRGBA == &image.straightRGBA) invalid("Repaint into a separate buffer");
    r.repaint(image, straightRGBA);
}
std::shared_ptr<const PaintedTextLayout> LayerRasterizer::textLayout(const std::string&, std::uint64_t) const { impl_->onThread(); return nullptr; }
std::shared_ptr<const LayerRasterImage> LayerRasterizer::refreshTypography(const std::string& id, std::uint64_t revision) {
    auto& r = *impl_; r.onThread(); const auto it = r.entries.find(id); if (it == r.entries.end() || !it->second.source) return {};
    const auto source = it->second.source; const bool releasable = it->second.releasable;
    return r.raster(id, revision, *source->layer, source->options, releasable, source);
}
bool LayerRasterizer::remove(const std::string& id) {
    auto& r = *impl_; r.onThread(); const auto it = r.entries.find(id); if (it == r.entries.end()) return false;
    r.counts.resourceBytes -= it->second.image->pixelBytes(); if (it->second.image->pixelsReleased()) r.releasedBytes -= it->second.image->pixelBytes();
    r.counts.typographyDescriptorBytes -= it->second.descriptorBytes; r.entries.erase(it); return true;
}
void LayerRasterizer::clear() { auto& r = *impl_; r.onThread(); r.entries.clear(); r.images.clear(); r.counts.resourceBytes = 0; r.releasedBytes = 0; r.counts.typographyDescriptorBytes = 0; }
void LayerRasterizer::forEachEntry(const std::function<void(const LayerRasterEntryUsage&)>& visit) const {
    const auto& r = *impl_; r.onThread();
    for (const auto& [id, e] : r.entries) visit({id, e.owner, e.image ? e.image->straightRGBA.capacity() : 0, 0, e.descriptorBytes, 0, false, e.image && e.image->pixelsReleased() ? e.image->pixelBytes() : 0});
}
std::size_t LayerRasterizer::decodedImageBytes() const { const auto& r = *impl_; r.onThread(); std::size_t bytes{}; for (const auto& [_, image] : r.images) bytes += image.premultipliedBGRA.capacity(); return bytes; }
LayerRasterStats LayerRasterizer::stats() const {
    const auto& r = *impl_; r.onThread(); auto result = r.counts; result.entries = r.entries.size(); result.decodedImages = r.images.size();
    for (const auto& [id, e] : r.entries) { if (e.image->pixelsReleased()) result.releasedPixelBytes += e.image->pixelBytes(); if (e.image->recipe_ && !e.hasText) result.repaintSourceBytes += e.image->recipe_->bytes; }
    return result;
}

// Raster attribution and memory relief, extracted from the Windows rasterizer at build time.
#include "raster_relief.inc"
} // namespace endfield::native
