#include "hud_shell.hpp"

#include "core/data/file_io.hpp"
#include "core/localization.hpp"
#include "modules/module_titles.hpp"
#include "native/watch_appearance.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace endfield::linux_port {
namespace gpu = native;
namespace source = core::source;
using Json = ehud::data::Json;
namespace {
// Action numbers for the fixed source buttons, above every exported navigation action.
constexpr std::uint64_t quitAction = ~std::uint64_t{0}, closeAction = quitAction - 1, terminalAction = quitAction - 2;
Json loadJSON(const std::filesystem::path& file, std::size_t limit) {
    const auto bytes = ehud::data::detail::readFile(file, limit);
    if (!bytes) throw std::runtime_error("Cannot read " + file.string());
    return Json::parse(*bytes, limit);
}
} // namespace

struct HudShell::Impl {
    source::WatchRuntimeInput input;
    source::WatchAnimation animation;
    app::SourceWatchSession session;
    gpu::SourceScene materials;
    gpu::WatchMaterialPresentation materialPresentation;
    gpu::LayerRasterizer& rasterizer;
    gpu::LayerScene layers;
    gpu::LayerRasterOptions raster;
    source::NativeLabelPlan labelPlan;
    std::unique_ptr<gpu::WatchContentCatalog> catalog;
    std::unique_ptr<gpu::WatchAppearanceTemplates> templates;
    std::unique_ptr<gpu::NativeWatchAppearance> appearance;
    std::unique_ptr<gpu::WatchLabelPresentation> labels;
    std::vector<gpu::WatchButtonAvailability> available;
    std::unique_ptr<source::DesktopChromeProjectionPlan> chromePlan;
    std::unique_ptr<gpu::NativeChromePresentation> chrome;
    gpu::DesktopChromeContent chromeContent;
    gpu::WatchAppearance look;
    gpu::LayerComposition composition;
    ShellSize size;
    double contentScale{1};
    // The text and chrome plane's own fade (DesktopChromeTiming), from the last open or close.
    bool closing{};
    double changedAt{}, capturedOpacity{};
    double canvasOpacity(double time) const {
        return source::DesktopChromeTiming::opacity(!closing, time - changedAt, capturedOpacity, false, chromePlan ? chromePlan->exitSeconds() : 0.);
    }
    app::WatchSessionEnvironment environment;
    gpu::DesktopBackdropAnimation backdrop;
    bool uploaded{}, composed{};
    std::uint64_t contentRevision{}, resourceRevision{};

    static std::vector<source::NativeLabelBinding> bindings(const source::WatchRuntimeInput& input) {
        const auto& top = input.nativeTop();
        return source::nativeLabelBindingsFromExport(input.scene(), input.nativeButtons(), top["nativeLayers"], top["nativeProfileBindings"]);
    }

    Impl(const std::filesystem::path& resources, gpu::LayerRasterizer& r, core::Language language)
        : input(resources / "shell" / "runtime-input"), animation(input.scene(), input.library()),
          session(input.scene(), input.document(), input.library(), input.camera(), input.resources(), input.animators(),
                  input.controllerTransitions(), input.profile(), input.desktopSettings()),
          materials(gpu::CompiledSourceScene{resources / "shell" / "catalog.ehscene", {}}), materialPresentation(materials),
          rasterizer(r), layers(r), labelPlan(input.scene(), bindings(input)) {
        backdrop = gpu::DesktopBackdropAnimation::fromSource(loadJSON(resources / "shell" / "watch-blur.json", 1024 * 1024));
        environment.presented = environment.onScreen = environment.canAdvanceTransition = true;
        if (materials.provenance().packetSHA256 != input.sourceManifestSHA256()) throw std::runtime_error("Compiled materials come from another package");
        look.language = language;
        raster.assetRoot = input.assetRoot();
        const Json& top = input.nativeTop();
        const Json& chromeReference = input.chrome();
        layers.load(chromeReference.isNull() ? top["nativeLayers"] : gpu::NativeChromePresentation::combinedReferenceRoot(top["nativeLayers"], chromeReference), raster);

        const std::array snapshots{gpu::WatchContentSnapshot{&top, 1}, gpu::WatchContentSnapshot{&input.nativeBottom(), 0}};
        catalog = std::make_unique<gpu::WatchContentCatalog>(input.scene(), input.document(), labelPlan.bindings(), snapshots);
        const auto& navigation = catalog->navigation();
        app::WatchSessionNavigation bound{navigation.fixedActions, navigation.rightActions, navigation.managedButtons};
        if (const auto& profile = input.profile())
            if (const auto action = catalog->actionForTarget("profile")) {
                bound.fixedActions[profile->rootID] = *action;
                for (const auto& id : profile->buttonIDs) bound.fixedActions[id] = *action;
            }
        for (const auto& node : input.scene().nodes()) {
            if (!input.document().component("UIButton", node.id)) continue;
            const std::string_view path = node.path;
            if (node.name == "QuitBtn") bound.fixedActions.try_emplace(node.id, quitAction);
            else if (path.ends_with("/CloseButtonNode/Btn_BackNode") || path.ends_with("/FullScreenCloseBtn")) bound.fixedActions.try_emplace(node.id, closeAction);
        }
        // A terminal tile leads the right-hand list.
        bound.rightActions.insert(bound.rightActions.begin(), terminalAction);
        session.setNavigation(std::move(bound), 0);

        const auto manifest = resources / "common" / "watch-appearance" / "manifest.json";
        templates = std::make_unique<gpu::WatchAppearanceTemplates>(loadJSON(manifest, 1024 * 1024));
        auto options = raster; options.assetRoot = manifest.parent_path();
        appearance = std::make_unique<gpu::NativeWatchAppearance>(*templates, input.scene(), labelPlan.bindings(), layers, rasterizer, options);
        appearance->setIconGlyphObserver([this](std::string_view id, core::Rect glyph) { (void)labelPlan.setIconGlyph(id, glyph); });
        std::vector<gpu::WatchAppearanceEntry> entries;
        for (const auto& e : catalog->entries())
            entries.push_back({e.action, e.target, modules::moduleTitle(e.target, language).value_or(e.title), "module:" + e.target, true});
        entries.push_back({terminalAction, "terminal", "Terminal", "shortcut:terminal", false});
        appearance->setEntries(entries);
        labels = std::make_unique<gpu::WatchLabelPresentation>(labelPlan, layers);
        for (const auto& id : labelPlan.buttonIDs()) available.push_back({id, false, false});

        if (!chromeReference.isNull()) {
            auto chromeBindings = source::DesktopChromeBindings::fromJson(chromeReference["bindings"]);
            chromePlan = std::make_unique<source::DesktopChromeProjectionPlan>(input.scene(), input.camera(), animation, chromeBindings);
            chrome = std::make_unique<gpu::NativeChromePresentation>(*chromePlan, layers, chromeReference, raster, rasterizer);
            chromeContent.reading = source::DesktopClockReading{"12:34:56", "WED Oct 7"};
            chromeContent.uppercaseShortcut = "CTRL + TAB";
            chromeContent.localizedClose = "CLICK OUTSIDE TO CLOSE";
            chrome->setContent(chromeContent);
        }
    }
};

HudShell::HudShell(const std::filesystem::path& resources, gpu::LayerRasterizer& rasterizer, core::Language language)
    : impl_(std::make_unique<Impl>(resources, rasterizer, language)) {}
HudShell::~HudShell() = default;
app::SourceWatchSession& HudShell::session() noexcept { return impl_->session; }
void HudShell::open(double time, std::uint64_t ambientSeed) {
    auto& s = *impl_;
    s.capturedOpacity = s.session.phase() == core::VisibilityPhase::concealed ? 0 : s.canvasOpacity(time);
    s.closing = false; s.changedAt = time; s.session.open(time, ambientSeed);
}
void HudShell::close(double time) {
    auto& s = *impl_;
    s.capturedOpacity = s.canvasOpacity(time); s.closing = true; s.changedAt = time; s.session.close(time);
}
void HudShell::resize(ShellSize size, double time) {
    auto& s = *impl_; s.size = size;
    const core::Rect viewport{0, 0, size.width / size.scale, size.height / size.scale};
    s.contentScale = s.chromePlan ? s.chromePlan->contentScale(viewport) : 1;
    s.environment.viewport = {viewport.width, viewport.height};
    s.session.setEnvironment(s.environment, time);
}
void HudShell::pointer(std::optional<core::Point> point, double time) {
    auto& s = *impl_;
    s.environment.pointer = point; s.session.setEnvironment(s.environment, time); s.session.pointerMove(point, time);
}
// HUDSourceWatchView.mouseDown: a press over a source control stays with it; empty space outside the
// displayed circle (radius 310 around design point 500,320 on the centre plane) closes the HUD.
bool HudShell::press(core::Point point, double time) {
    auto& s = *impl_;
    pointer(point, time);
    if (!shown() || s.closing) return false;
    if (!s.session.sourceControlAt(point) && s.chromePlan && s.chromePlan->projection().center) {
        const auto plane = core::Projection::viewport(gpu::layerViewportProjection(s.size.width, s.size.height) *
            core::Matrix4::scale(s.size.scale, s.size.scale, 1) * *s.chromePlan->projection().center, s.size.width, s.size.height);
        if (const auto design = plane.unproject({point.x * s.size.scale, point.y * s.size.scale})) {
            const double dx = design->x - 500, dy = design->y - 320;
            if (dx * dx + dy * dy >= 310. * 310.) return true;
        }
    }
    s.session.pointerDown(point, time);
    return false;
}
std::string HudShell::release(core::Point point, double time) {
    auto& s = *impl_;
    const auto activation = s.session.pointerUp(point, time);
    if (!activation) return {};
    if (activation->action == quitAction) return "quit";
    if (activation->action == closeAction) return "close";
    if (activation->action == terminalAction) return "terminal";
    for (const auto& e : s.catalog->entries()) if (e.action == activation->action) return e.target;
    return {};
}
bool HudShell::wheel(core::Point point, double steps, double time) { return impl_->session.wheel(point, steps, 3, time); }
bool HudShell::shown() const noexcept { return impl_->session.phase() != core::VisibilityPhase::concealed; }
double HudShell::backdropOpacity(double time) const {
    const auto& s = *impl_;
    if (!shown()) return 0;
    const auto& track = s.closing ? s.backdrop.exit : s.backdrop.entrance;
    return track.alpha(time - s.changedAt);
}
void HudShell::setReading(std::string time, std::string date, std::string shortcut) {
    auto& s = *impl_; if (!s.chrome) return;
    s.chromeContent.reading = source::DesktopClockReading{std::move(time), std::move(date)};
    s.chromeContent.uppercaseShortcut = std::move(shortcut);
    s.chrome->setContent(s.chromeContent);
}
void HudShell::invalidate() noexcept { impl_->uploaded = impl_->composed = false; }
bool HudShell::frame(gpu::Renderer& renderer, double time) {
    auto& s = *impl_;
    const auto* sample = s.session.sample(time);
    if (!sample) return false;
    if (sample->visibility.phase == core::VisibilityPhase::visible && !s.session.inputEnabled()) s.session.setInputEnabled(true, time);
    const double width = s.size.width / s.size.scale, height = s.size.height / s.size.scale;
    const core::Rect viewport{0, 0, width, height};
    auto parameters = s.materials.parameters();
    parameters.camera = sample->gpuCamera; parameters.timeSeconds = sample->shaderTime;
    parameters.width = s.size.width; parameters.height = s.size.height;
    s.materialPresentation.update(*sample->sourceFrame, parameters);
    for (auto& button : s.available) button.enabled = s.session.actions().find(button.buttonID) != s.session.actions().end();
    s.labels->update(*sample->sourceFrame, sample->camera, viewport, s.available);
    s.appearance->update(s.session.actions(), s.look, s.labels->placements());
    if (s.chrome) {
        source::DesktopChromeSettings settings{viewport, 1, {}, core::Module::power, true};
        settings.contentScale = s.contentScale;
        s.chrome->advance(time);
        s.chrome->update(*sample->sourceFrame, sample->camera, settings, static_cast<float>(s.canvasOpacity(time)));
    }
    if (!s.uploaded) { s.materials.upload(renderer.sourceGraphics()); s.uploaded = true; }
    else s.materials.flush(renderer.sourceGraphics());

    std::vector<gpu::LayerCompositionEntry> order{{&s.layers, {}}};
    if (s.chrome) if (auto* pages = s.chrome->transitionScene()) order.push_back({pages, {}});
    if (!s.composed || order.size() != s.composition.sceneCount() || s.layers.contentRevision() != s.contentRevision || s.layers.resourceRevision() != s.resourceRevision) {
        s.composition.setEntries(renderer, order);
        s.composed = true; s.contentRevision = s.layers.contentRevision(); s.resourceRevision = s.layers.resourceRevision();
    }
    s.composition.present(renderer);
    renderer.setCamera(gpu::layerViewportProjection(s.size.width, s.size.height) * core::Matrix4::scale(s.size.scale, s.size.scale, 1));
    return true;
}
} // namespace endfield::linux_port
