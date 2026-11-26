// The window side of the OpenGL renderer: it owns the context and shows finished frames.
#pragma once

namespace endfield::linux_port {
class GraphicsHost {
public:
    virtual ~GraphicsHost() = default;
    // Makes the context current on the calling thread.
    virtual void makeCurrent() = 0;
    // `texture`: premultiplied RGBA8 with row 0 at the top of the image.
    virtual void present(unsigned texture, unsigned width, unsigned height) = 0;
};
} // namespace endfield::linux_port
