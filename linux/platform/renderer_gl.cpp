// OpenGL 4.5 implementation of endfield::native::Renderer (windows/native/renderer.hpp).
// Offscreen target only so far. Draw validation, the object uniform layout and the texture
// conversions come from the Direct3D renderer through the generated include below.
// Not implemented: composition surfaces, media textures, worker textures, content filter,
// source material pass and device suspension.
#include "renderer_logic.cpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

namespace endfield::native {
namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw RendererError("Read shader " + path.string(), -1);
    std::ostringstream text; text << in.rdbuf(); return text.str();
}

// Loads a shader, inlining scene.glsl and inserting variant definitions after #version.
std::string shaderSource(const std::filesystem::path& directory, const char* name, const std::string& defines) {
    std::istringstream in(readText(directory / name));
    std::string line, out;
    while (std::getline(in, line)) {
        if (line.rfind("#extension GL_GOOGLE_include_directive", 0) == 0) continue;
        if (line.rfind("#include \"scene.glsl\"", 0) == 0) { out += readText(directory / "scene.glsl"); continue; }
        out += line + "\n";
        if (line.rfind("#version", 0) == 0) out += defines;
    }
    return out;
}

GLuint compileStage(GLenum stage, const std::string& source) {
    const GLuint shader = glCreateShader(stage);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
    GLint ok{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); glDeleteShader(shader); throw RendererError(std::string("Compile shader: ") + log, -1); }
    return shader;
}

GLuint linkProgram(const std::string& vertex, const std::string& fragment) {
    const GLuint v = compileStage(GL_VERTEX_SHADER, vertex), f = compileStage(GL_FRAGMENT_SHADER, fragment), program = glCreateProgram();
    glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok{}; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]{}; glGetProgramInfoLog(program, sizeof(log), nullptr, log); throw RendererError(std::string("Link shader: ") + log, -1); }
    return program;
}

GLuint newTarget(unsigned width, unsigned height, GLenum format, GLuint& framebuffer) {
    GLuint texture{};
    glCreateTextures(GL_TEXTURE_2D, 1, &texture); glTextureStorage2D(texture, 1, format, width, height);
    for (auto [name, value] : {std::pair{GL_TEXTURE_MIN_FILTER, GL_LINEAR}, std::pair{GL_TEXTURE_MAG_FILTER, GL_LINEAR},
                               std::pair{GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE}, std::pair{GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE}})
        glTextureParameteri(texture, name, value);
    glCreateFramebuffers(1, &framebuffer); glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0);
    require(glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete render target");
    return texture;
}
} // namespace

struct RendererParkedTexture::Impl {
    bool held{true};
    std::uint64_t revision{};
    std::vector<std::uint8_t> texels; // as stored on the GPU
    std::uint32_t width{}, height{};
    TextureColorSpace colorSpace{};
    TextureFilter filter{};
    bool alphaOnly{}, updatable{};
    std::size_t gpuBytes{};
};

RendererParkedTexture::RendererParkedTexture(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
RendererParkedTexture::~RendererParkedTexture() = default;
bool RendererParkedTexture::held() const noexcept { return impl_->held; }
std::uint64_t RendererParkedTexture::revision() const noexcept { return impl_->revision; }
std::size_t RendererParkedTexture::bytes() const noexcept { return impl_->held ? impl_->texels.size() : 0; }

struct Renderer::Impl {
    struct Group;
    struct Mesh {
        GLuint vertices{}, indices{}, array{};
        GLsizei indexCount{};
        std::uint64_t revision{};
        std::size_t bytes{};
        Group* group{};
        Mesh() = default;
        Mesh(Mesh&& other) noexcept { *this = std::move(other); }
        Mesh& operator=(Mesh&& o) noexcept {
            std::swap(vertices, o.vertices); std::swap(indices, o.indices); std::swap(array, o.array);
            indexCount = o.indexCount; revision = o.revision; bytes = o.bytes; group = o.group; return *this;
        }
        ~Mesh() { if (array) glDeleteVertexArrays(1, &array); if (vertices) glDeleteBuffers(1, &vertices); if (indices) glDeleteBuffers(1, &indices); }
    };
    struct Texture {
        GLuint name{};
        std::uint32_t width{}, height{};
        TextureColorSpace colorSpace{TextureColorSpace::sRGB};
        TextureFilter filter{TextureFilter::linear};
        bool alphaOnly{}, updatable{}, encoded{};
        std::uint64_t revision{};
        std::size_t bytes{};
        Group* group{};
        Texture() = default;
        Texture(Texture&& other) noexcept { *this = std::move(other); }
        Texture& operator=(Texture&& o) noexcept {
            std::swap(name, o.name); width = o.width; height = o.height; colorSpace = o.colorSpace; filter = o.filter;
            alphaOnly = o.alphaOnly; updatable = o.updatable; encoded = o.encoded; revision = o.revision; bytes = o.bytes; group = o.group; return *this;
        }
        ~Texture() { if (name) glDeleteTextures(1, &name); }
    };
    struct Draw {
        std::string sourceID;
        const Mesh* mesh{};
        const Texture* texture{};
        const Texture* alphaMask{};
        ObjectUniform values{};
        NativeBlend blend{};
    };
    struct Group {
        NativeGroupTarget requested;
        DrawObject output;
        std::vector<Draw> draws;
        GLuint framebuffer{}, camera{};
        unsigned width{}, height{}, depth{1};
        std::size_t bytes{};
        bool dirty{true}, visible{}, ahead{};
        ~Group() { if (framebuffer) glDeleteFramebuffers(1, &framebuffer); if (camera) glDeleteBuffers(1, &camera); }
    };

    EGLDisplay display{EGL_NO_DISPLAY};
    EGLContext context{EGL_NO_CONTEXT};
    std::uint32_t width{}, height{};
    GLuint sampleLinear{}, sampleNearest{}, cameraBuffer{}, objectBuffer{}, emptyArray{};
    GLuint linearFramebuffer{}, linearTexture{}, outputFramebuffer{}, outputTexture{};
    GLuint scene[6]{}, composite{};
    std::map<std::string, Mesh> meshes;
    std::map<std::string, Texture> textures;
    std::map<std::string, std::unique_ptr<Group>> groups;
    std::vector<Draw> draws;
    Texture white;
    std::vector<std::weak_ptr<RendererParkedTexture::Impl>> parked;
    bool readbackReady{};
    RendererStats counters;

    void current() const { require(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE, "Activate graphics context"); }

    void createContext() {
        // The extension entry point is used because core EGL 1.5 functions cannot be resolved before a display exists.
        if (epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless"))
            display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "Open EGL display");
        }
        require(eglBindAPI(EGL_OPENGL_API), "Select OpenGL");
        const EGLint attributes[]{EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
        require(context != EGL_NO_CONTEXT, "Create OpenGL 4.5 context");
        current();
    }

    void createTargets() {
        for (GLuint* f : {&linearFramebuffer, &outputFramebuffer}) if (*f) { glDeleteFramebuffers(1, f); *f = 0; }
        for (GLuint* t : {&linearTexture, &outputTexture}) if (*t) { glDeleteTextures(1, t); *t = 0; }
        linearTexture = newTarget(width, height, GL_RGBA16F, linearFramebuffer);
        outputTexture = newTarget(width, height, GL_RGBA8, outputFramebuffer);
        readbackReady = false;
    }

    static std::size_t storedBytes(const TextureData& d, bool alphaOnly) { return std::size_t(d.width) * d.height * (alphaOnly ? 1 : 4); }

    static std::vector<std::uint8_t> convert(std::span<const std::uint8_t> rgba, TextureColorSpace space, bool alphaOnly) {
        std::vector<std::uint8_t> out(alphaOnly ? rgba.size() / 4 : rgba.size());
        if (alphaOnly) detail::prepareTextureAlpha8(rgba, out);
        else if (space == TextureColorSpace::sRGB) detail::prepareTextureSRGB8(rgba, out);
        else if (space == TextureColorSpace::linear) detail::prepareTextureLinear8(rgba, out);
        else detail::prepareTextureEncoded8(rgba, out);
        return out;
    }

    static Texture makeTexture(std::uint32_t w, std::uint32_t h, TextureColorSpace space, TextureFilter filter, bool alphaOnly, const std::uint8_t* stored) {
        Texture t;
        t.width = w; t.height = h; t.colorSpace = space; t.filter = filter; t.alphaOnly = alphaOnly;
        t.encoded = !alphaOnly && space == TextureColorSpace::encodedSRGB;
        glCreateTextures(GL_TEXTURE_2D, 1, &t.name);
        glTextureStorage2D(t.name, 1, alphaOnly ? GL_R8 : t.encoded ? GL_RGBA8 : GL_SRGB8_ALPHA8, w, h);
        if (alphaOnly) { // reads as (0, 0, 0, a), like a Direct3D alpha-only texture
            const GLint swizzle[4]{GL_ZERO, GL_ZERO, GL_ZERO, GL_RED};
            glTextureParameteriv(t.name, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(t.name, 0, 0, 0, w, h, alphaOnly ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, stored);
        t.bytes = std::size_t(w) * h * (alphaOnly ? 1 : 4);
        return t;
    }

    static Mesh makeMesh(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices) {
        Mesh m;
        glCreateBuffers(1, &m.vertices); glNamedBufferStorage(m.vertices, vertices.size_bytes(), vertices.data(), 0);
        glCreateBuffers(1, &m.indices); glNamedBufferStorage(m.indices, indices.size_bytes(), indices.data(), 0);
        glCreateVertexArrays(1, &m.array);
        glVertexArrayVertexBuffer(m.array, 0, m.vertices, 0, sizeof(Vertex));
        glVertexArrayElementBuffer(m.array, m.indices);
        const std::pair<int, std::size_t> layout[]{{3, offsetof(Vertex, position)}, {2, offsetof(Vertex, uv)}, {4, offsetof(Vertex, linearColor)}};
        for (GLuint i = 0; i < 3; ++i) {
            glEnableVertexArrayAttrib(m.array, i);
            glVertexArrayAttribFormat(m.array, i, layout[i].first, GL_FLOAT, GL_FALSE, static_cast<GLuint>(layout[i].second));
            glVertexArrayAttribBinding(m.array, i, 0);
        }
        m.indexCount = static_cast<GLsizei>(indices.size());
        return m;
    }

    void budget(std::size_t previous, std::size_t next) const {
        require(next <= Renderer::maximumResourceBytes && counters.resourceBytes - previous <= Renderer::maximumResourceBytes - next, "Retained GPU resource budget exceeded");
    }
    template <class F> bool anyDraw(F predicate) const {
        for (const auto& d : draws) if (predicate(d)) return true;
        for (const auto& [id, g] : groups) for (const auto& d : g->draws) if (predicate(d)) return true;
        return false;
    }
    void invalidate(const Mesh* mesh) { for (auto& [id, g] : groups) for (const auto& d : g->draws) if (d.mesh == mesh) g->dirty = true; }
    void invalidate(const Texture* texture) { for (auto& [id, g] : groups) for (const auto& d : g->draws) if (d.texture == texture || d.alphaMask == texture) g->dirty = true; }

    // Validates the whole list before returning it, so a rejected list changes nothing.
    std::vector<Draw> resolve(std::span<const DrawObject> objects, bool localGroup, NativeGroupColorSpace space) {
        require(objects.size() <= Renderer::maximumObjects, "Retained draw count exceeds bounds");
        validateNativeBlends(objects, localGroup, space);
        std::vector<Draw> result; result.reserve(objects.size());
        for (const auto& object : objects) {
            Draw d; d.sourceID = object.sourceID; d.values = uniforms(object); d.blend = object.blend;
            const auto mesh = meshes.find(object.meshID);
            require(mesh != meshes.end(), "Draw refers to an unregistered mesh");
            d.mesh = &mesh->second;
            if (object.textureID.empty()) d.texture = &white;
            else { const auto t = textures.find(object.textureID); require(t != textures.end(), "Draw refers to an unregistered texture"); d.texture = &t->second; }
            d.alphaMask = &white;
            if (object.alphaMask) { const auto t = textures.find(object.alphaMask->textureID); require(t != textures.end(), "Draw refers to an unregistered alpha-mask texture"); d.alphaMask = &t->second; }
            result.push_back(std::move(d));
        }
        return result;
    }
    static bool same(const std::vector<Draw>& a, const std::vector<Draw>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].mesh != b[i].mesh || a[i].texture != b[i].texture || a[i].alphaMask != b[i].alphaMask || a[i].blend != b[i].blend ||
                a[i].sourceID != b[i].sourceID || std::memcmp(&a[i].values, &b[i].values, sizeof(ObjectUniform))) return false;
        return true;
    }
    // A group may borrow only a leaf's output, and only in encoded colour space.
    unsigned inputDepth(std::span<const Draw> list, const Group* configured, NativeGroupColorSpace space) const {
        unsigned depth = 1;
        for (const auto& d : list) {
            const Group* child = d.mesh->group;
            require(child == d.texture->group, "Native group output mesh and texture must belong to the same group");
            require(d.alphaMask->group == nullptr, "Native group cannot consume a group alpha-mask texture");
            if (!child) continue;
            require(child != configured, "Native group cannot consume its own output");
            require(space == NativeGroupColorSpace::encodedSRGB && child->requested.colorSpace == NativeGroupColorSpace::encodedSRGB, "Nested groups require source encoded color space");
            require(child->depth == 1, "Native group nesting exceeds two levels");
            require(d.blend == NativeBlend::sourceOver, "Nested group output uses source-over composition");
            depth = 2;
        }
        if (configured && configured->depth == 1 && depth == 2)
            for (const auto& [id, parent] : groups) if (parent.get() != configured)
                for (const auto& d : parent->draws) require(d.mesh->group != configured, "A borrowed leaf cannot become a nested parent");
        return depth;
    }

    void render(const std::vector<Draw>& list, GLuint framebuffer, GLuint camera, unsigned w, unsigned h, bool encodedOutput) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, w, h);
        const float clear[4]{}; glClearNamedFramebufferfv(framebuffer, GL_COLOR, 0, clear);
        glEnable(GL_BLEND);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, camera);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, objectBuffer);
        for (const auto& d : list) {
            if (d.values.opacity == 0 || d.values.tint[3] == 0) continue;
            if (d.blend == NativeBlend::screen) glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            // Program order: plain, encoded input, encoded output, both, alpha, alpha with encoded output.
            const int program = d.texture->alphaOnly ? (encodedOutput ? 5 : 4) : (d.texture->encoded ? 1 : 0) + (encodedOutput ? 2 : 0);
            glUseProgram(scene[program]);
            glNamedBufferSubData(objectBuffer, 0, sizeof(ObjectUniform), &d.values);
            glBindTextureUnit(0, d.texture->name); glBindTextureUnit(1, d.alphaMask->name);
            glBindSampler(0, d.texture->filter == TextureFilter::nearest ? sampleNearest : sampleLinear);
            glBindSampler(1, sampleLinear);
            glBindVertexArray(d.mesh->array);
            glDrawElements(GL_TRIANGLES, d.mesh->indexCount, GL_UNSIGNED_INT, nullptr);
            ++counters.drawCalls;
        }
    }

    bool configure(const std::string& id, const NativeGroupTarget& requested, std::optional<std::span<const DrawObject>> objects) {
        identity(id); require(id.size() <= 480, "Native group ID exceeds generated-identity bounds");
        const auto& b = requested.localBounds; const double density = requested.pixelsPerPoint;
        require(requested.colorSpace == NativeGroupColorSpace::linear || requested.colorSpace == NativeGroupColorSpace::encodedSRGB, "Unknown native group color space");
        require(std::isfinite(density) && density > 0 && density <= 16, "Invalid native group density");
        require(std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) && std::isfinite(b.height) && b.width > 0 && b.height > 0, "Invalid native group local bounds");
        const double left = std::floor(b.x * density), top = std::floor(b.y * density), right = std::ceil((b.x + b.width) * density), bottom = std::ceil((b.y + b.height) * density);
        require(right > left && bottom > top && right - left <= 8192 && bottom - top <= 8192 && (right - left) * (bottom - top) <= Renderer::maximumNativeGroupPixels, "Native group pixel bounds exceed limits");
        const unsigned w = unsigned(right - left), h = unsigned(bottom - top);
        auto found = groups.find(id);
        require(found == groups.end() || found->second->requested.colorSpace == requested.colorSpace, "Retire native group before changing its color space");
        std::vector<Draw> list; unsigned depth = found == groups.end() ? 1 : found->second->depth;
        if (objects) { list = resolve(*objects, true, requested.colorSpace); depth = inputDepth(list, found == groups.end() ? nullptr : found->second.get(), requested.colorSpace); }
        if (found != groups.end() && found->second->requested == requested) {
            if (!objects || same(list, found->second->draws)) return false;
            found->second->draws = std::move(list); found->second->depth = depth; found->second->dirty = true; return true;
        }
        const bool existing = found != groups.end();
        require(existing || groups.size() < Renderer::maximumNativeGroups, "Native group count exceeds limits");
        const std::string output = "native-group:" + id;
        if (!existing) require(!meshes.contains(output + "/quad") && !textures.contains(output + "/color"), "Native group generated identity collides with a retained resource");
        const bool encoded = requested.colorSpace == NativeGroupColorSpace::encodedSRGB;
        const std::size_t bytes = std::size_t(w) * h * (encoded ? 4 : 8);
        std::size_t total = 0; for (const auto& [name, g] : groups) if (!existing || g.get() != found->second.get()) total += g->bytes;
        require(bytes <= Renderer::maximumNativeGroupBytes && total <= Renderer::maximumNativeGroupBytes - bytes, "Native group aggregate target budget exceeded");
        const std::size_t previous = existing ? found->second->bytes : 0; budget(previous, bytes);
        Group* group = existing ? found->second.get() : groups.emplace(id, std::make_unique<Group>()).first->second.get();
        const double cx = left / density, cy = top / density, cw = w / density, ch = h / density;
        const float x = float(cx), y = float(cy), r = float(cx + cw), q = float(cy + ch);
        const std::array<Vertex, 4> corners{{{{x, y, 0}, {0, 0}, {1, 1, 1, 1}}, {{r, y, 0}, {1, 0}, {1, 1, 1, 1}}, {{r, q, 0}, {1, 1}, {1, 1, 1, 1}}, {{x, q, 0}, {0, 1}, {1, 1, 1, 1}}}};
        constexpr std::array<std::uint32_t, 6> order{0, 1, 2, 0, 2, 3};
        Mesh quad = makeMesh(corners, order); quad.group = group;
        if (group->framebuffer) glDeleteFramebuffers(1, &group->framebuffer);
        Texture color; color.name = newTarget(w, h, encoded ? GL_RGBA8 : GL_RGBA16F, group->framebuffer);
        color.width = w; color.height = h; color.encoded = encoded; color.bytes = bytes; color.group = group;
        core::Matrix4 camera; camera.values = {2. / cw, 0, 0, 0, 0, -2. / ch, 0, 0, 0, 0, 0, 0, -1 - 2 * cx / cw, 1 + 2 * cy / ch, 0, 1};
        const auto values = matrix(camera);
        if (!group->camera) { glCreateBuffers(1, &group->camera); glNamedBufferStorage(group->camera, sizeof(values), nullptr, GL_DYNAMIC_STORAGE_BIT); }
        glNamedBufferSubData(group->camera, 0, sizeof(values), values.data());
        group->requested = requested; group->width = w; group->height = h; group->bytes = bytes; group->depth = depth; group->dirty = true;
        group->output.sourceID = output; group->output.meshID = output + "/quad"; group->output.textureID = output + "/color";
        if (objects) group->draws = std::move(list);
        // Assigning into existing map nodes keeps published pointers valid.
        meshes[group->output.meshID] = std::move(quad); textures[group->output.textureID] = std::move(color);
        counters.resourceBytes = counters.resourceBytes - previous + bytes; ++counters.nativeGroupTargetAllocations;
        return true;
    }

    ~Impl() {
        if (context == EGL_NO_CONTEXT) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        draws.clear(); groups.clear(); meshes.clear(); textures.clear(); white = Texture{};
        for (GLuint p : scene) if (p) glDeleteProgram(p);
        if (composite) glDeleteProgram(composite);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;
bool Renderer::shadersEmbedded() noexcept { return false; }

void Renderer::initialize(void* window, std::uint32_t width, std::uint32_t height, const RendererOptions& options) {
    require(options.target == RenderTarget::offscreenForTests && window == nullptr, "Only the offscreen target is implemented");
    dimensions(width, height);
    auto next = std::make_unique<Impl>();
    next->createContext();
    require(epoxy_gl_version() >= 45, "OpenGL 4.5 is required");
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE); // Direct3D's top-left rows and 0..1 depth
    glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glDisable(GL_FRAMEBUFFER_SRGB);
    const auto dir = options.shaderPath;
    const std::string vertex = shaderSource(dir, "scene.vert", "");
    const char* variants[6]{"", "#define ENCODED_INPUT\n", "#define ENCODED_OUTPUT\n", "#define ENCODED_INPUT\n#define ENCODED_OUTPUT\n",
                            "#define ALPHA_INPUT\n", "#define ALPHA_INPUT\n#define ENCODED_OUTPUT\n"};
    // A single-channel input is already linear, so only the output encoding varies for it.
    for (int i = 0; i < 6; ++i) next->scene[i] = linkProgram(vertex, shaderSource(dir, "scene.frag", variants[i]));
    next->composite = linkProgram(shaderSource(dir, "composite.vert", ""), shaderSource(dir, "composite.frag", ""));
    for (GLuint* s : {&next->sampleLinear, &next->sampleNearest}) {
        glCreateSamplers(1, s);
        const GLint filter = s == &next->sampleLinear ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(*s, GL_TEXTURE_MIN_FILTER, filter); glSamplerParameteri(*s, GL_TEXTURE_MAG_FILTER, filter);
        glSamplerParameteri(*s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glSamplerParameteri(*s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    const std::array<float, 16> identityMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glCreateBuffers(1, &next->cameraBuffer); glNamedBufferStorage(next->cameraBuffer, sizeof(identityMatrix), identityMatrix.data(), GL_DYNAMIC_STORAGE_BIT);
    glCreateBuffers(1, &next->objectBuffer); glNamedBufferStorage(next->objectBuffer, sizeof(ObjectUniform), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateVertexArrays(1, &next->emptyArray);
    const std::uint8_t white[4]{255, 255, 255, 255};
    next->white = Impl::makeTexture(1, 1, TextureColorSpace::sRGB, TextureFilter::linear, false, white);
    next->width = width; next->height = height; next->createTargets();
    next->counters.initialized = true;
    impl_ = std::move(next);
}

void Renderer::resize(std::uint32_t width, std::uint32_t height) {
    require(impl_ != nullptr, "Renderer is not initialized"); dimensions(width, height); impl_->current();
    if (width == impl_->width && height == impl_->height) return;
    impl_->width = width; impl_->height = height; impl_->createTargets();
}

bool Renderer::setMesh(std::string sourceID, std::uint64_t revision, MeshData input) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current(); identity(sourceID);
    const auto existing = r.meshes.find(sourceID);
    require(existing == r.meshes.end() || !existing->second.group, "Native group output mesh is managed by its group");
    if (existing != r.meshes.end() && existing->second.revision == revision) return false;
    require(existing != r.meshes.end() || r.meshes.size() < maximumMeshes, "Retained mesh count exceeds bounds");
    require(!input.vertices.empty() && input.vertices.size() <= 1000000 && !input.indices.empty() && input.indices.size() <= 3000000 && input.indices.size() % 3 == 0, "Invalid triangle mesh size");
    for (const auto& v : input.vertices) {
        for (float value : v.position) require(std::isfinite(value), "Nonfinite mesh position");
        for (float value : v.uv) require(std::isfinite(value), "Nonfinite mesh UV");
        for (float value : v.linearColor) require(std::isfinite(value) && value >= 0 && value <= 1, "Invalid linear vertex color");
    }
    for (auto i : input.indices) require(i < input.vertices.size(), "Triangle index exceeds vertex count");
    const auto bytes = input.vertices.size_bytes() + input.indices.size_bytes();
    const auto previous = existing == r.meshes.end() ? 0 : existing->second.bytes;
    r.budget(previous, bytes);
    auto mesh = Impl::makeMesh(input.vertices, input.indices); mesh.revision = revision; mesh.bytes = bytes;
    auto& stored = r.meshes[std::move(sourceID)]; stored = std::move(mesh); r.invalidate(&stored);
    r.counters.resourceBytes = r.counters.resourceBytes - previous + bytes; ++r.counters.meshUploads; r.readbackReady = false;
    return true;
}

bool Renderer::setTexture(std::string sourceID, std::uint64_t revision, TextureData input) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current(); identity(sourceID);
    const auto existing = r.textures.find(sourceID);
    require(existing == r.textures.end() || !existing->second.group, "Native group texture is managed by its owner");
    if (existing != r.textures.end() && existing->second.revision == revision) return false;
    require(existing != r.textures.end() || r.textures.size() < maximumTextures, "Retained texture count exceeds bounds");
    require(input.width > 0 && input.height > 0 && input.width <= 8192 && input.height <= 8192 && std::uint64_t(input.width) * input.height * 4 == input.straightRGBA.size(), "Invalid straight RGBA8 texture dimensions");
    require((input.colorSpace == TextureColorSpace::sRGB || input.colorSpace == TextureColorSpace::linear || input.colorSpace == TextureColorSpace::encodedSRGB) &&
            (input.filter == TextureFilter::nearest || input.filter == TextureFilter::linear), "Invalid texture sampling policy");
    const auto previous = existing == r.textures.end() ? 0 : existing->second.bytes;
    const bool alphaOnly = detail::alphaOnlyTexels(input.straightRGBA, input.updatable);
    const auto bytes = Impl::storedBytes(input, alphaOnly); r.budget(previous, bytes);
    const auto stored = Impl::convert(input.straightRGBA, input.colorSpace, alphaOnly);
    auto texture = Impl::makeTexture(input.width, input.height, input.colorSpace, input.filter, alphaOnly, stored.data());
    texture.revision = revision; texture.updatable = input.updatable;
    auto& slot = r.textures[std::move(sourceID)]; slot = std::move(texture); r.invalidate(&slot);
    r.counters.resourceBytes = r.counters.resourceBytes - previous + bytes; ++r.counters.textureUploads; r.readbackReady = false;
    return true;
}

bool Renderer::updateTexture(const std::string& sourceID, std::uint64_t revision, std::uint32_t x, std::uint32_t y, TextureData region) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    const auto found = r.textures.find(sourceID);
    require(found != r.textures.end() && found->second.updatable && !found->second.group, "Only an updatable texture installed by setTexture is painted in place");
    auto& t = found->second;
    if (t.revision == revision) return false;
    require(region.width > 0 && region.height > 0 && x <= t.width && y <= t.height && region.width <= t.width - x && region.height <= t.height - y &&
            std::uint64_t(region.width) * region.height * 4 == region.straightRGBA.size(), "Texture region lies outside its texture or has invalid RGBA8 dimensions");
    require(region.colorSpace == t.colorSpace, "A texture region keeps its texture's color space");
    if (t.alphaOnly && !detail::alphaOnlyTexels(region.straightRGBA, false)) {
        // A coloured region widens the one-byte texture to four bytes, keeping its texels.
        r.budget(t.bytes, t.bytes * 4);
        std::vector<std::uint8_t> alpha(std::size_t(t.width) * t.height), wide(alpha.size() * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTextureImage(t.name, 0, GL_RED, GL_UNSIGNED_BYTE, GLsizei(alpha.size()), alpha.data());
        for (std::size_t i = 0; i < alpha.size(); ++i) { wide[i * 4] = wide[i * 4 + 1] = wide[i * 4 + 2] = 255; wide[i * 4 + 3] = alpha[i]; }
        const auto stored = Impl::convert(wide, t.colorSpace, false);
        auto next = Impl::makeTexture(t.width, t.height, t.colorSpace, t.filter, false, stored.data()); next.updatable = true;
        r.counters.resourceBytes = r.counters.resourceBytes - t.bytes + next.bytes; t = std::move(next);
    }
    const auto stored = Impl::convert(region.straightRGBA, t.colorSpace, t.alphaOnly);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(t.name, 0, x, y, region.width, region.height, t.alphaOnly ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, stored.data());
    t.revision = revision; r.invalidate(&t); ++r.counters.textureRegionUploads; r.counters.textureRegionBytes += region.straightRGBA.size(); r.readbackReady = false;
    return true;
}

bool Renderer::removeMesh(const std::string& sourceID) {
    if (!impl_) return false; auto& r = *impl_; r.current(); const auto item = r.meshes.find(sourceID);
    if (item == r.meshes.end() || item->second.group || r.anyDraw([&](const Impl::Draw& d) { return d.mesh == &item->second; })) return false;
    r.counters.resourceBytes -= item->second.bytes; r.meshes.erase(item); return true;
}
bool Renderer::removeTexture(const std::string& sourceID) {
    if (!impl_) return false; auto& r = *impl_; r.current(); const auto item = r.textures.find(sourceID);
    if (item == r.textures.end() || item->second.group || r.anyDraw([&](const Impl::Draw& d) { return d.texture == &item->second || d.alphaMask == &item->second; })) return false;
    r.counters.resourceBytes -= item->second.bytes; r.textures.erase(item); return true;
}
std::optional<std::uint64_t> Renderer::textureRevision(const std::string& sourceID) const {
    if (!impl_) return std::nullopt; const auto item = impl_->textures.find(sourceID);
    if (item == impl_->textures.end()) return std::nullopt; return item->second.revision;
}

std::shared_ptr<RendererParkedTexture> Renderer::parkTexture(const std::string& sourceID) {
    if (!impl_) return nullptr; auto& r = *impl_; r.current(); const auto item = r.textures.find(sourceID);
    if (item == r.textures.end()) return nullptr; auto& t = item->second;
    if (t.group || t.bytes > maximumParkedBytes || r.anyDraw([&](const Impl::Draw& d) { return d.texture == &t || d.alphaMask == &t; })) return nullptr;
    // The oldest copies are dropped first to stay within the parked bound.
    std::size_t held = 0; for (const auto& weak : r.parked) if (auto c = weak.lock(); c && c->held) held += c->texels.size();
    for (const auto& weak : r.parked) { if (held <= maximumParkedBytes - t.bytes) break; if (auto c = weak.lock(); c && c->held) { held -= c->texels.size(); c->held = false; c->texels = {}; ++r.counters.parkedDrops; } }
    auto copy = std::make_shared<RendererParkedTexture::Impl>();
    copy->texels.resize(t.bytes); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureImage(t.name, 0, t.alphaOnly ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, GLsizei(t.bytes), copy->texels.data());
    copy->revision = t.revision; copy->width = t.width; copy->height = t.height; copy->colorSpace = t.colorSpace; copy->filter = t.filter;
    copy->alphaOnly = t.alphaOnly; copy->updatable = t.updatable; copy->gpuBytes = t.bytes;
    r.parked.push_back(copy); r.counters.resourceBytes -= t.bytes; r.textures.erase(item); ++r.counters.textureParks;
    return std::shared_ptr<RendererParkedTexture>(new RendererParkedTexture(std::move(copy)));
}
bool Renderer::restoreTexture(const std::string& sourceID, RendererParkedTexture& handle) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current(); identity(sourceID);
    auto& copy = *handle.impl_;
    bool ours = false; for (const auto& weak : r.parked) if (weak.lock().get() == &copy) ours = true;
    if (!ours || !copy.held || r.textures.contains(sourceID)) return false;
    require(r.textures.size() < maximumTextures, "Retained texture count exceeds bounds"); r.budget(0, copy.gpuBytes);
    auto t = Impl::makeTexture(copy.width, copy.height, copy.colorSpace, copy.filter, copy.alphaOnly, copy.texels.data());
    t.revision = copy.revision; t.updatable = copy.updatable;
    r.textures[sourceID] = std::move(t); r.counters.resourceBytes += copy.gpuBytes; ++r.counters.parkedRestores;
    copy.held = false; copy.texels = {}; return true;
}

bool Renderer::configureNativeGroup(std::string id, const NativeGroupTarget& target) {
    require(impl_ != nullptr, "Renderer is not initialized"); impl_->current(); return impl_->configure(id, target, std::nullopt);
}
bool Renderer::configureNativeGroup(std::string id, const NativeGroupTarget& target, std::span<const DrawObject> objects) {
    require(impl_ != nullptr, "Renderer is not initialized"); impl_->current(); return impl_->configure(id, target, objects);
}
bool Renderer::setNativeGroupDraws(const std::string& id, std::span<const DrawObject> objects) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    const auto found = r.groups.find(id); require(found != r.groups.end(), "Unknown native group");
    auto list = r.resolve(objects, true, found->second->requested.colorSpace);
    const auto depth = r.inputDepth(list, found->second.get(), found->second->requested.colorSpace);
    found->second->depth = depth;
    if (Impl::same(list, found->second->draws)) return false;
    found->second->draws = std::move(list); found->second->dirty = true; r.readbackReady = false; return true;
}
const DrawObject& Renderer::nativeGroupOutput(const std::string& id) const {
    require(impl_ != nullptr, "Renderer is not initialized"); const auto found = impl_->groups.find(id);
    require(found != impl_->groups.end(), "Unknown native group"); return found->second->output;
}
bool Renderer::removeNativeGroup(const std::string& id) {
    if (!impl_) return false; auto& r = *impl_; r.current(); const auto found = r.groups.find(id);
    if (found == r.groups.end()) return false; auto* group = found->second.get();
    for (const auto& d : r.draws) if (d.mesh->group == group || d.texture->group == group) return false;
    for (const auto& [name, other] : r.groups) if (other.get() != group) for (const auto& d : other->draws) if (d.mesh->group == group) return false;
    const auto output = group->output; r.counters.resourceBytes -= group->bytes;
    r.groups.erase(found); r.meshes.erase(output.meshID); r.textures.erase(output.textureID); return true;
}
bool Renderer::setNativeGroupRenderAhead(const std::string& id, bool ahead) {
    if (!impl_) return false; const auto found = impl_->groups.find(id);
    if (found == impl_->groups.end()) return false; found->second->ahead = ahead; return true;
}

void Renderer::setDrawList(std::span<const DrawObject> objects) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    r.draws = r.resolve(objects, false, NativeGroupColorSpace::linear); r.readbackReady = false;
}
void Renderer::setCamera(const core::Matrix4& viewProjection) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    const auto values = matrix(viewProjection);
    glNamedBufferSubData(r.cameraBuffer, 0, sizeof(values), values.data()); ++r.counters.cameraUploads; r.readbackReady = false;
}

void Renderer::draw(bool present) {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    require(!present, "An isolated render target cannot present to the desktop");
    for (auto& [id, g] : r.groups) g->visible = false;
    const auto expose = [](const Impl::Draw& d) {
        if (d.values.opacity <= 0 || d.values.tint[3] <= 0) return;
        if (d.texture->group) d.texture->group->visible = true;
        if (d.alphaMask->group) d.alphaMask->group->visible = true;
    };
    for (const auto& d : r.draws) expose(d);
    for (auto& [id, g] : r.groups) if (g->visible && g->depth == 2) for (const auto& d : g->draws) expose(d);
    std::size_t ahead = 0;
    for (unsigned depth = 1; depth <= 2; ++depth)
        for (auto& [id, g] : r.groups) {
            if (g->depth != depth || !g->dirty) continue;
            if (!g->visible && (!g->ahead || ahead++ >= 2)) continue;
            r.render(g->draws, g->framebuffer, g->camera, g->width, g->height, g->requested.colorSpace == NativeGroupColorSpace::encodedSRGB);
            g->dirty = false; ++r.counters.nativeGroupRenders;
            r.invalidate(&r.textures.at(g->output.textureID));
        }
    r.render(r.draws, r.linearFramebuffer, r.cameraBuffer, r.width, r.height, false);
    // Linear premultiplied composition converted to encoded premultiplied output.
    glBindFramebuffer(GL_FRAMEBUFFER, r.outputFramebuffer); glViewport(0, 0, r.width, r.height); glDisable(GL_BLEND);
    glUseProgram(r.composite); glBindTextureUnit(0, r.linearTexture); glBindSampler(0, r.sampleNearest);
    glBindVertexArray(r.emptyArray); glDrawArrays(GL_TRIANGLES, 0, 3);
    require(glGetError() == GL_NO_ERROR, "OpenGL reported an error while drawing");
    r.readbackReady = true;
}

Readback Renderer::readback() {
    require(impl_ != nullptr, "Renderer is not initialized"); auto& r = *impl_; r.current();
    require(r.readbackReady, "Readback requires a completed render-only draw");
    Readback result; result.width = r.width; result.height = r.height; result.rowBytes = r.width * 4;
    result.pixels.resize(std::size_t(result.rowBytes) * result.height);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureImage(r.outputTexture, 0, GL_BGRA, GL_UNSIGNED_BYTE, GLsizei(result.pixels.size()), result.pixels.data());
    return result;
}

void Renderer::clearDrawList() { if (impl_) { impl_->draws.clear(); impl_->readbackReady = false; } }
void Renderer::clearResources() {
    if (!impl_) return; auto& r = *impl_; r.current();
    r.draws.clear(); r.groups.clear(); r.meshes.clear(); r.textures.clear(); r.parked.clear(); r.counters.resourceBytes = 0; r.readbackReady = false;
}
void Renderer::reset() noexcept { impl_.reset(); }
RendererStats Renderer::stats() const noexcept {
    if (!impl_) return {};
    auto s = impl_->counters; s.meshes = impl_->meshes.size(); s.textures = impl_->textures.size(); s.objects = impl_->draws.size();
    s.nativeGroups = impl_->groups.size(); for (const auto& [id, g] : impl_->groups) s.nativeGroupBytes += g->bytes;
    return s;
}
} // namespace endfield::native
