// The source shell of the HUD: dial materials, navigation captions and icons, header, clock and
// footer, assembled from an official release's resources the way the Windows application does
// (windows/app/application.cpp construct() and poseShell()). No window, input source or clock.
#pragma once
#include "app/source_watch_session.hpp"
#include "core/watch_runtime_input.hpp"
#include "native/chrome_presentation.hpp"
#include "native/desktop_backdrop.hpp"
#include "native/layer_scene.hpp"
#include "native/renderer.hpp"
#include "native/source_scene.hpp"
#include "native/watch_content.hpp"
#include "native/watch_presentation.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace endfield::linux_port {
struct ShellSize { unsigned width{}, height{}; double scale{1}; };

class HudShell final {
public:
    // `resources`: the resources directory of an official release. `rasterizer`, and any renderer
    // passed to frame(), outlive the shell.
    HudShell(const std::filesystem::path& resources, native::LayerRasterizer& rasterizer, core::Language language);
    ~HudShell();
    HudShell(const HudShell&) = delete;
    HudShell& operator=(const HudShell&) = delete;
    app::SourceWatchSession& session() noexcept;
    void open(double time, std::uint64_t ambientSeed);
    void close(double time);
    // A new pixel size or scale; takes effect on the next frame.
    void resize(ShellSize, double time);
    // The clock reading and shortcut name shown by the header and footer.
    void setReading(std::string time, std::string date, std::string shortcut);
    // Pointer events in logical points of the window.
    void pointer(std::optional<core::Point>, double time);
    // True when the press lands on empty space outside the dial, which closes the HUD.
    bool press(core::Point, double time);
    // The action of the source button released on, as a module name, "terminal", "quit" or "close".
    std::string release(core::Point, double time);
    bool wheel(core::Point, double steps, double time);
    // False once a close has finished.
    bool shown() const noexcept;
    // The desktop dimming's own fade (the original WatchBlur timing).
    double backdropOpacity(double time) const;
    // Samples the session at `time` and submits one frame. False while nothing is shown.
    bool frame(native::Renderer&, double time);
    // Uploads again on the next frame (a new or reset renderer).
    void invalidate() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace endfield::linux_port
