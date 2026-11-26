// Draws a finished HUD frame into the window's framebuffer over the dimming backdrop.
#pragma once

namespace endfield::linux_port {
struct Backdrop {
    double opacity{};  // the backdrop's own fade
    double darkness{}; // strength of the flat tint
};
class Presenter final {
public:
    Presenter() = default;
    // Release with the context current.
    void release() noexcept;
    void draw(unsigned framebuffer, unsigned texture, unsigned width, unsigned height, Backdrop);
private:
    unsigned program_{}, array_{};
};
} // namespace endfield::linux_port
