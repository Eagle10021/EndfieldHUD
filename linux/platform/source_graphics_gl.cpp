// OpenGL implementation of endfield::native::SourceGraphics (windows/native/source_graphics.hpp):
// the ordered original material passes. The compiled scene carries the Direct3D shader bytecode
// of the Windows build; libvkd3d-shader translates it to SPIR-V, which OpenGL loads directly.
// Resource limits and validation follow windows/native/source_graphics.cpp.
// The owning renderer keeps the OpenGL context current for every call.
#include "native/renderer.hpp"
#include "native/source_graphics.hpp"

#include <epoxy/gl.h>
#include <vkd3d_shader.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace endfield::native {
namespace {
void need(bool okay, const char* message) { if (!okay) throw std::invalid_argument(message); }
void idCheck(const std::string& id) { need(!id.empty() && id.size() <= 512, "Invalid original resource identity"); }
constexpr std::size_t maxBytes = 512ull * 1024 * 1024;
// Constant buffers: vertex slots occupy bindings 0..13, fragment slots 14..27.
constexpr unsigned stageSlots = 14;

GLenum blendFactor(unsigned metal) {
    static constexpr GLenum values[]{GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE};
    need(metal < std::size(values), "Unsupported original blend factor"); return values[metal];
}
GLenum blendOperation(unsigned metal) {
    static constexpr GLenum values[]{GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    need(metal < std::size(values), "Unsupported original blend operation"); return values[metal];
}
GLenum comparison(unsigned metal) { need(metal < 8, "Invalid source comparison"); return GL_NEVER + metal; }
GLenum stencilOperation(unsigned metal) {
    static constexpr GLenum values[]{GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    need(metal < std::size(values), "Invalid source stencil operation"); return values[metal];
}
GLenum address(unsigned metal) {
    static constexpr GLenum values[]{GL_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_BORDER};
    need(metal < std::size(values), "Unsupported source sampler address mode"); return values[metal];
}
struct PixelFormat { GLenum internal, external; unsigned texelBytes; bool compressed; };
PixelFormat pixelFormat(const std::string& name) {
    static const std::map<std::string, PixelFormat> formats{
        {"rgba8Unorm", {GL_RGBA8, GL_RGBA, 4, false}}, {"rgba8Unorm_srgb", {GL_SRGB8_ALPHA8, GL_RGBA, 4, false}},
        {"bgra8Unorm", {GL_RGBA8, GL_BGRA, 4, false}}, {"bgra8Unorm_srgb", {GL_SRGB8_ALPHA8, GL_BGRA, 4, false}},
        {"r8Unorm", {GL_R8, GL_RED, 1, false}},
        {"bc7_rgbaUnorm", {GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 16, true}}, {"bc7_rgbaUnorm_srgb", {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 0, 16, true}}};
    const auto it = formats.find(name); need(it != formats.end(), "Unsupported original pixel format"); return it->second;
}
double seconds() noexcept { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Timed { double& total; double begin{seconds()}; ~Timed() { total += seconds() - begin; } };

struct Messages { char* text{}; ~Messages() { vkd3d_shader_free_messages(text); } };
[[noreturn]] void translationError(const char* operation, int code, const Messages& messages) {
    throw RendererError(std::string(operation) + ": " + (messages.text ? messages.text : "no message"), code);
}

vkd3d_shader_compile_info compileInfo(std::span<const std::uint8_t> bytecode) {
    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.source = {bytecode.data(), bytecode.size()};
    info.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.log_level = VKD3D_SHADER_LOG_ERROR;
    return info;
}
// A sampled (texture, sampler) register pair and the texture unit assigned to it.
struct Sampled { SourceStage stage; unsigned texture, sampler, unit; };
constexpr unsigned unsampled = VKD3D_SHADER_DUMMY_SAMPLER_INDEX;

// Input registers of a vertex program by TEXCOORD semantic index.
std::map<unsigned, unsigned> inputRegisters(std::span<const std::uint8_t> bytecode) {
    auto info = compileInfo(bytecode);
    vkd3d_shader_scan_signature_info signature{};
    signature.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO; info.next = &signature;
    Messages messages;
    if (const int code = vkd3d_shader_scan(&info, &messages.text); code < 0) translationError("Read original vertex inputs", code, messages);
    std::map<unsigned, unsigned> result;
    for (unsigned i = 0; i < signature.input.element_count; ++i) {
        const auto& e = signature.input.elements[i];
        if (std::string_view(e.semantic_name) == "TEXCOORD") result[e.semantic_index] = e.register_index;
    }
    vkd3d_shader_free_scan_signature_info(&signature);
    return result;
}

GLuint translate(std::span<const std::uint8_t> bytecode, SourceStage stage, std::vector<Sampled>& sampled, unsigned& units) {
    const auto visibility = stage == SourceStage::vertex ? VKD3D_SHADER_VISIBILITY_VERTEX : VKD3D_SHADER_VISIBILITY_PIXEL;
    auto scan = compileInfo(bytecode);
    vkd3d_shader_scan_combined_resource_sampler_info pairs{};
    pairs.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_COMBINED_RESOURCE_SAMPLER_INFO; scan.next = &pairs;
    vkd3d_shader_scan_descriptor_info descriptors{};
    descriptors.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO; pairs.next = &descriptors;
    Messages scanMessages;
    if (const int code = vkd3d_shader_scan(&scan, &scanMessages.text); code < 0) translationError("Read original shader resources", code, scanMessages);
    std::vector<vkd3d_shader_resource_binding> buffers;
    bool supported = true;
    for (unsigned i = 0; i < descriptors.descriptor_count; ++i) {
        const auto& d = descriptors.descriptors[i];
        if (d.type == VKD3D_SHADER_DESCRIPTOR_TYPE_CBV) {
            if (d.register_space != 0 || d.register_index >= stageSlots) { supported = false; continue; }
            vkd3d_shader_resource_binding b{};
            b.type = d.type; b.register_index = d.register_index; b.shader_visibility = visibility; b.flags = VKD3D_SHADER_BINDING_FLAG_BUFFER;
            b.binding = {0, d.register_index + (stage == SourceStage::vertex ? 0 : stageSlots), 1};
            buffers.push_back(b);
        } else if (d.type == VKD3D_SHADER_DESCRIPTOR_TYPE_UAV) supported = false;
    }
    std::vector<vkd3d_shader_combined_resource_sampler> combined;
    for (unsigned i = 0; i < pairs.combined_sampler_count; ++i) {
        const auto& p = pairs.combined_samplers[i];
        if (p.resource_space != 0 || (p.sampler_index != unsampled && p.sampler_space != 0)) { supported = false; continue; }
        vkd3d_shader_combined_resource_sampler c{};
        c.resource_index = p.resource_index; c.sampler_index = p.sampler_index; c.shader_visibility = visibility;
        c.flags = VKD3D_SHADER_BINDING_FLAG_IMAGE; c.binding = {0, units, 1};
        combined.push_back(c); sampled.push_back({stage, p.resource_index, p.sampler_index, units++});
    }
    vkd3d_shader_free_scan_combined_resource_sampler_info(&pairs);
    vkd3d_shader_free_scan_descriptor_info(&descriptors);
    need(supported, "Original shader uses unsupported resources");

    vkd3d_shader_interface_info interface{};
    interface.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;
    interface.bindings = buffers.data(); interface.binding_count = unsigned(buffers.size());
    interface.combined_samplers = combined.data(); interface.combined_sampler_count = unsigned(combined.size());
    vkd3d_shader_spirv_target_info target{};
    target.type = VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;
    target.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_OPENGL_4_5; target.next = &interface;
    auto info = compileInfo(bytecode); info.next = &target;
    const vkd3d_shader_compile_option options[]{{VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_10}};
    info.options = options; info.option_count = 1;
    vkd3d_shader_code spirv{}; Messages messages;
    if (const int code = vkd3d_shader_compile(&info, &spirv, &messages.text); code < 0) translationError("Translate original shader", code, messages);
    const GLuint shader = glCreateShader(stage == SourceStage::vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.code, GLsizei(spirv.size));
    vkd3d_shader_free_shader_code(&spirv);
    glSpecializeShader(shader, "main", 0, nullptr, nullptr);
    GLint ok{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); glDeleteShader(shader); throw RendererError(std::string("Load translated shader: ") + log, -1); }
    return shader;
}
} // namespace

struct SourceGraphics::Impl {
    struct Mesh {
        GLuint vertices{}, indices{}; unsigned stride{}, indexCount{}; std::size_t bytes{}, vertexCapacity{}, indexCapacity{}; std::uint64_t revision{};
        std::vector<std::uint32_t> indexData;
    };
    struct Texture { GLuint name{}, sampler{}, nearest{}; std::size_t bytes{}; std::uint64_t revision{}; };
    struct Pipeline {
        GLuint program{}, array{}; unsigned vertexBytes{}; std::vector<Sampled> sampled;
        bool blendEnabled{}, depthWrite{}, stencilEnabled{}; unsigned writeMask{}, cull{};
        std::array<GLenum, 6> blend{}; GLenum depthCompare{}; SourceStencilFace front, back;
    };
    struct Uniform { GLuint buffer{}; std::vector<std::uint8_t> bytes; };
    struct Constant { unsigned binding; Uniform* value; };
    struct Image { unsigned unit; Texture* value; GLuint sampler; };
    struct Draw { Mesh* mesh; Pipeline* pipeline; unsigned firstIndex, indexCount, stencilReference; std::vector<Constant> constants; std::vector<Image> images; };

    std::thread::id owner{std::this_thread::get_id()};
    std::map<std::string, Mesh> meshes; std::map<std::string, Texture> textures;
    std::map<std::string, Pipeline> pipelines; std::map<std::string, Uniform> uniforms;
    std::vector<Draw> draws;
    GLuint color{}, depth{}, framebuffer{};
    unsigned width{}, height{};
    SourceGraphicsStats counters;
    bool suspended{};
    void thread() const { need(owner == std::this_thread::get_id(), "Source GPU calls require their owner thread"); }
    void live() const { thread(); need(!suspended, "The original pass's device is suspended"); }
    void budget(std::size_t previous, std::size_t incoming) const {
        need(incoming <= maxBytes && counters.payloadBytes - previous <= maxBytes - incoming, "Original GPU resource budget exceeded");
    }
    void releaseTargetObjects() noexcept {
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (color) glDeleteTextures(1, &color);
        if (depth) glDeleteTextures(1, &depth);
        framebuffer = color = depth = 0; width = height = 0;
    }
    static void release(Texture& t) noexcept { glDeleteTextures(1, &t.name); glDeleteSamplers(1, &t.sampler); glDeleteSamplers(1, &t.nearest); }
    void releaseObjects() noexcept {
        draws.clear();
        for (auto& [id, m] : meshes) { glDeleteBuffers(1, &m.vertices); glDeleteBuffers(1, &m.indices); }
        for (auto& [id, t] : textures) release(t);
        for (auto& [id, p] : pipelines) { glDeleteProgram(p.program); glDeleteVertexArrays(1, &p.array); }
        for (auto& [id, u] : uniforms) glDeleteBuffers(1, &u.buffer);
        meshes.clear(); textures.clear(); pipelines.clear(); uniforms.clear(); releaseTargetObjects(); counters.payloadBytes = 0;
    }
    void targets(unsigned w, unsigned h) {
        need(w && h && std::uint64_t(w) * h <= Renderer::maximumRenderPixels, "Invalid original target extent");
        if (w == width && h == height) return;
        releaseTargetObjects();
        glCreateTextures(GL_TEXTURE_2D, 1, &color); glTextureStorage2D(color, 1, GL_SRGB8_ALPHA8, w, h);
        glCreateTextures(GL_TEXTURE_2D, 1, &depth); glTextureStorage2D(depth, 1, GL_DEPTH32F_STENCIL8, w, h);
        glCreateFramebuffers(1, &framebuffer);
        glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, color, 0);
        glNamedFramebufferTexture(framebuffer, GL_DEPTH_STENCIL_ATTACHMENT, depth, 0);
        need(glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Create original LDR target");
        width = w; height = h;
    }
};

std::size_t SourceGraphics::releaseTargets() noexcept {
    auto& r = *impl_;
    if (r.owner != std::this_thread::get_id() || !r.color) return 0;
    const auto bytes = std::size_t(r.width) * r.height * 12;
    r.releaseTargetObjects(); return bytes;
}
// The device and context arguments belong to the Direct3D build; OpenGL uses the current context.
SourceGraphics::SourceGraphics(void*, void*) : impl_(std::make_unique<Impl>()) {}
SourceGraphics::~SourceGraphics() { impl_->releaseObjects(); }

void SourceGraphics::setMesh(const std::string& id, std::uint64_t revision, unsigned stride,
                             std::span<const std::uint8_t> vertices, std::span<const std::uint32_t> indices) {
    auto& r = *impl_; r.live(); idCheck(id); const Timed timed{r.counters.uploadSeconds};
    auto old = r.meshes.find(id); if (old != r.meshes.end() && old->second.revision == revision) return;
    need(old != r.meshes.end() || r.meshes.size() < 4096, "Original mesh count exceeds bounds");
    need(stride >= 8 && stride <= 256 && !vertices.empty() && vertices.size() % stride == 0 &&
         vertices.size() / stride <= 1000000 && !indices.empty() && indices.size() <= 3000000 && indices.size() % 3 == 0,
         "Invalid original mesh layout");
    for (auto index : indices) need(index < vertices.size() / stride, "Original index exceeds mesh");
    if (old != r.meshes.end())
        for (const auto& draw : r.draws) if (draw.mesh == &old->second)
            need(stride >= draw.pipeline->vertexBytes && draw.firstIndex <= indices.size() && draw.indexCount <= indices.size() - draw.firstIndex,
                 "Original mesh replacement would invalidate a retained draw");
    const auto capacity = [](std::size_t previous, std::size_t requested) {
        if (requested <= previous) return previous; std::size_t next = std::max<std::size_t>(previous, 16);
        while (next < requested) { if (next > maxBytes / 2) return requested; next *= 2; } return next;
    };
    const bool fresh = old == r.meshes.end();
    const auto vertexCapacity = capacity(fresh ? 0 : old->second.vertexCapacity, vertices.size_bytes());
    const auto indexCapacity = capacity(fresh ? 0 : old->second.indexCapacity, indices.size_bytes());
    const auto size = vertexCapacity + indexCapacity, previous = fresh ? 0 : old->second.bytes; r.budget(previous, size);
    Impl::Mesh& mesh = fresh ? r.meshes[id] : old->second;
    const bool newVertex = fresh || vertexCapacity != mesh.vertexCapacity, newIndex = fresh || indexCapacity != mesh.indexCapacity;
    const bool changedIndex = fresh || !std::equal(indices.begin(), indices.end(), mesh.indexData.begin(), mesh.indexData.end());
    if (fresh) { glCreateBuffers(1, &mesh.vertices); glCreateBuffers(1, &mesh.indices); }
    // Orphaning lets the driver rename storage that a submitted frame still reads.
    glNamedBufferData(mesh.vertices, GLsizeiptr(vertexCapacity), nullptr, GL_DYNAMIC_DRAW);
    glNamedBufferSubData(mesh.vertices, 0, GLsizeiptr(vertices.size_bytes()), vertices.data());
    if (changedIndex || newIndex) {
        glNamedBufferData(mesh.indices, GLsizeiptr(indexCapacity), nullptr, GL_DYNAMIC_DRAW);
        glNamedBufferSubData(mesh.indices, 0, GLsizeiptr(indices.size_bytes()), indices.data());
        mesh.indexData.assign(indices.begin(), indices.end());
    }
    mesh.stride = stride; mesh.indexCount = unsigned(indices.size()); mesh.bytes = size;
    mesh.vertexCapacity = vertexCapacity; mesh.indexCapacity = indexCapacity; mesh.revision = revision;
    r.counters.payloadBytes += size - previous; ++r.counters.geometryUploads; ++r.counters.vertexUploads; if (changedIndex) ++r.counters.indexUploads;
    r.counters.meshBufferAllocations += std::uint64_t(newVertex) + std::uint64_t(newIndex);
}

void SourceGraphics::setTexture(std::string id, std::uint64_t revision, const SourceTexture& input) {
    auto& r = *impl_; r.live(); idCheck(id); const Timed timed{r.counters.uploadSeconds};
    auto old = r.textures.find(id); if (old != r.textures.end() && old->second.revision == revision) return;
    need(old != r.textures.end() || r.textures.size() < 2048, "Original texture count exceeds bounds");
    need(!input.mips.empty() && input.mips.size() <= 15, "Invalid source mip count");
    const auto format = pixelFormat(input.pixelFormat);
    const auto& base = input.mips[0]; need(base.width && base.height && base.width <= 16384 && base.height <= 16384, "Invalid source texture dimensions");
    std::size_t size{};
    for (unsigned i = 0; i < input.mips.size(); ++i) {
        const auto& mip = input.mips[i];
        const auto rows = format.compressed ? (mip.height + 3) / 4 : mip.height;
        const auto pitch = format.compressed ? (mip.width + 3) / 4 * 16 : mip.width * format.texelBytes;
        need(mip.width == std::max(1u, base.width >> i) && mip.height == std::max(1u, base.height >> i) &&
             mip.rowBytes == pitch && mip.bytes.size() == std::size_t(rows) * pitch, "Original mip data mismatch");
        need(mip.bytes.size() <= maxBytes - size, "Original texture too large"); size += mip.bytes.size();
    }
    const auto previous = old == r.textures.end() ? 0 : old->second.bytes; r.budget(previous, size);
    need(input.minFilter <= 1 && input.magFilter <= 1 && input.mipFilter <= 2 && input.maxAnisotropy >= 1 && input.maxAnisotropy <= 16, "Invalid original sampler filters");
    const GLenum wrap[3]{address(input.addressU), address(input.addressV), address(input.addressW)};
    Impl::Texture value;
    glCreateTextures(GL_TEXTURE_2D, 1, &value.name);
    glTextureStorage2D(value.name, GLsizei(input.mips.size()), format.internal, base.width, base.height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (unsigned i = 0; i < input.mips.size(); ++i) {
        const auto& mip = input.mips[i];
        if (format.compressed) glCompressedTextureSubImage2D(value.name, i, 0, 0, mip.width, mip.height, format.internal, GLsizei(mip.bytes.size()), mip.bytes.data());
        else glTextureSubImage2D(value.name, i, 0, 0, mip.width, mip.height, format.external, GL_UNSIGNED_BYTE, mip.bytes.data());
    }
    const GLint minification = !input.mipFilter ? (input.minFilter ? GL_LINEAR : GL_NEAREST)
        : input.mipFilter == 2 ? (input.minFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                               : (input.minFilter ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    glCreateSamplers(1, &value.sampler); glCreateSamplers(1, &value.nearest);
    for (const GLuint s : {value.sampler, value.nearest}) {
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[0]); glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[1]); glSamplerParameteri(s, GL_TEXTURE_WRAP_R, wrap[2]);
    }
    glSamplerParameteri(value.sampler, GL_TEXTURE_MIN_FILTER, minification);
    glSamplerParameteri(value.sampler, GL_TEXTURE_MAG_FILTER, input.magFilter ? GL_LINEAR : GL_NEAREST);
    if (input.maxAnisotropy > 1 && input.minFilter == 1 && input.magFilter == 1) glSamplerParameterf(value.sampler, GL_TEXTURE_MAX_ANISOTROPY, float(input.maxAnisotropy));
    // Used where a program fetches texels without a sampler.
    glSamplerParameteri(value.nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); glSamplerParameteri(value.nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    value.bytes = size; value.revision = revision;
    if (old == r.textures.end()) r.textures.emplace(std::move(id), value);
    else {
        // Draws keep the map entry and the sampler object names, which follow the replacement.
        auto replaced = old->second; old->second = value;
        for (auto& draw : r.draws) for (auto& image : draw.images) {
            if (image.sampler == replaced.sampler) image.sampler = value.sampler;
            else if (image.sampler == replaced.nearest) image.sampler = value.nearest;
        }
        Impl::release(replaced);
    }
    r.counters.payloadBytes += size - previous; ++r.counters.textureUploads;
}

void SourceGraphics::setPipeline(std::string id, const SourcePipeline& input) {
    auto& r = *impl_; r.live(); idCheck(id);
    need(!r.pipelines.contains(id), "An original pipeline identity is immutable");
    need(r.pipelines.size() < 2048 && !input.vertexBytecode.empty() && !input.fragmentBytecode.empty() &&
         input.vertexBytecode.size() <= 4 * 1024 * 1024 && input.fragmentBytecode.size() <= 4 * 1024 * 1024 && input.attributes.size() <= 16,
         "Invalid original shader program dimensions");
    Impl::Pipeline p;
    need(input.rgbOperation <= 4 && input.alphaOperation <= 4 && input.writeMask <= 15, "Invalid original blend operation or write mask");
    p.blendEnabled = input.blendEnabled;
    p.blend = {blendFactor(input.sourceRGB), blendFactor(input.destinationRGB), blendFactor(input.sourceAlpha), blendFactor(input.destinationAlpha),
               blendOperation(input.rgbOperation), blendOperation(input.alphaOperation)};
    p.writeMask = input.writeMask; p.depthCompare = comparison(input.depthCompare); p.depthWrite = input.depthWrite; p.stencilEnabled = input.stencilEnabled;
    need(input.front.readMask <= 255 && input.front.writeMask <= 255 && input.front.readMask == input.back.readMask && input.front.writeMask == input.back.writeMask,
         "Original front and back stencil masks must match");
    for (const auto* face : {&input.front, &input.back}) { comparison(face->compare); stencilOperation(face->fail); stencilOperation(face->depthFail); stencilOperation(face->pass); }
    p.front = input.front; p.back = input.back;
    need(input.cull <= 2, "Unsupported original cull mode"); p.cull = input.cull;
    std::set<unsigned> locations;
    for (const auto& value : input.attributes) {
        need(value.components >= 1 && value.components <= 4 && value.byteOffset <= 240 && value.location < 16 && locations.insert(value.location).second,
             "Invalid original vertex attribute");
        p.vertexBytes = std::max(p.vertexBytes, value.byteOffset + value.components * 4);
    }
    const auto registers = inputRegisters(input.vertexBytecode);
    unsigned units = 0;
    const GLuint vertex = translate(input.vertexBytecode, SourceStage::vertex, p.sampled, units);
    GLuint fragment = 0;
    try { fragment = translate(input.fragmentBytecode, SourceStage::fragment, p.sampled, units); }
    catch (...) { glDeleteShader(vertex); throw; }
    p.program = glCreateProgram();
    glAttachShader(p.program, vertex); glAttachShader(p.program, fragment); glLinkProgram(p.program);
    glDeleteShader(vertex); glDeleteShader(fragment);
    GLint ok{}; glGetProgramiv(p.program, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]{}; glGetProgramInfoLog(p.program, sizeof(log), nullptr, log); glDeleteProgram(p.program); throw RendererError(std::string("Link original program: ") + log, -1); }
    glCreateVertexArrays(1, &p.array);
    for (const auto& value : input.attributes) {
        const auto found = registers.find(value.location);
        if (found == registers.end()) continue; // the program does not read this attribute
        glEnableVertexArrayAttrib(p.array, found->second);
        glVertexArrayAttribFormat(p.array, found->second, value.components, GL_FLOAT, GL_FALSE, value.byteOffset);
        glVertexArrayAttribBinding(p.array, found->second, 0);
    }
    r.pipelines.emplace(std::move(id), std::move(p));
}

void SourceGraphics::setUniform(const std::string& id, std::span<const std::uint8_t> bytes) {
    auto& r = *impl_; r.live(); idCheck(id); const Timed timed{r.counters.uploadSeconds};
    need(!bytes.empty() && bytes.size() <= 65536 && bytes.size() % 16 == 0, "Original constant buffer must be bounded and 16-byte aligned");
    auto old = r.uniforms.find(id);
    if (old != r.uniforms.end() && std::equal(bytes.begin(), bytes.end(), old->second.bytes.begin(), old->second.bytes.end())) return;
    need(old != r.uniforms.end() || r.uniforms.size() < 8192, "Original uniform count exceeds bounds");
    const auto previous = old == r.uniforms.end() ? 0 : old->second.bytes.size(); r.budget(previous, bytes.size());
    auto& value = old == r.uniforms.end() ? r.uniforms[id] : old->second;
    if (!value.buffer) glCreateBuffers(1, &value.buffer);
    if (previous != bytes.size()) { glNamedBufferData(value.buffer, GLsizeiptr(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW); ++r.counters.uniformAllocations; }
    else glNamedBufferSubData(value.buffer, 0, GLsizeiptr(bytes.size()), bytes.data());
    value.bytes.assign(bytes.begin(), bytes.end());
    r.counters.payloadBytes += bytes.size() - previous; ++r.counters.uniformUploads;
}

void SourceGraphics::setDraws(std::span<const SourceDraw> inputs) {
    auto& r = *impl_; r.live(); need(inputs.size() <= 16384, "Too many original draw passes");
    std::vector<Impl::Draw> draws; draws.reserve(inputs.size());
    for (const auto& input : inputs) {
        auto mesh = r.meshes.find(input.mesh); auto pipeline = r.pipelines.find(input.pipeline);
        need(mesh != r.meshes.end() && pipeline != r.pipelines.end(), "Original draw resource is missing");
        need(mesh->second.stride >= pipeline->second.vertexBytes && input.firstIndex <= mesh->second.indexCount && input.indexCount > 0 &&
             input.indexCount <= mesh->second.indexCount - input.firstIndex && input.indexCount % 3 == 0 && input.stencilReference <= 255,
             "Invalid original draw range");
        std::set<std::pair<int, unsigned>> buffers, textures, samplers;
        Impl::Draw draw{&mesh->second, &pipeline->second, input.firstIndex, input.indexCount, input.stencilReference, {}, {}};
        for (const auto& value : input.uniforms) {
            need(value.stage == SourceStage::vertex || value.stage == SourceStage::fragment, "Invalid original uniform stage");
            need(value.slot < stageSlots && r.uniforms.contains(value.id) && buffers.emplace(int(value.stage), value.slot).second, "Invalid/duplicate original constant binding");
            draw.constants.push_back({value.slot + (value.stage == SourceStage::vertex ? 0 : stageSlots), &r.uniforms.at(value.id)});
        }
        for (const auto& value : input.textures) {
            need(value.stage == SourceStage::vertex || value.stage == SourceStage::fragment, "Invalid original texture stage");
            need(value.slot < 128 && value.samplerSlot < 16 && r.textures.contains(value.id) && textures.emplace(int(value.stage), value.slot).second &&
                 samplers.emplace(int(value.stage), value.samplerSlot).second, "Invalid/duplicate original texture binding");
        }
        // Each unit the program samples takes its texture register's image with its sampler register's state.
        for (const auto& pair : pipeline->second.sampled) {
            Impl::Texture* texture = nullptr; Impl::Texture* sampler = nullptr;
            for (const auto& value : input.textures) {
                if (value.stage != pair.stage) continue;
                if (value.slot == pair.texture) texture = &r.textures.at(value.id);
                if (value.samplerSlot == pair.sampler) sampler = &r.textures.at(value.id);
            }
            if (!texture) continue; // unbound registers read as zero
            draw.images.push_back({pair.unit, texture, pair.sampler == unsampled || !sampler ? texture->nearest : sampler->sampler});
        }
        draws.push_back(std::move(draw));
    }
    r.draws = std::move(draws);
}

void SourceGraphics::renderTo(void* destination, unsigned width, unsigned height) {
    auto& r = *impl_; r.live(); need(destination, "Missing original output target"); const Timed timed{r.counters.renderSeconds};
    // The destination is the name of an RGBA8 texture of exactly this size.
    const GLuint output = *static_cast<const GLuint*>(destination);
    GLint w{}, h{}, format{};
    glGetTextureLevelParameteriv(output, 0, GL_TEXTURE_WIDTH, &w); glGetTextureLevelParameteriv(output, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTextureLevelParameteriv(output, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    need(unsigned(w) == width && unsigned(h) == height && format == GL_RGBA8, "Original output target contract mismatch");
    r.targets(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, r.framebuffer); glViewport(0, 0, width, height);
    glEnable(GL_FRAMEBUFFER_SRGB); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE); glStencilMask(0xff);
    const float transparent[4]{};
    glClearNamedFramebufferfv(r.framebuffer, GL_COLOR, 0, transparent);
    glClearNamedFramebufferfi(r.framebuffer, GL_DEPTH_STENCIL, 0, 1.f, 0);
    glEnable(GL_DEPTH_TEST); glFrontFace(GL_CW); // Direct3D's clockwise front faces
    for (unsigned unit = 0; unit < 32; ++unit) { glBindTextureUnit(unit, 0); glBindSampler(unit, 0); }
    const Impl::Pipeline* bound = nullptr; const Impl::Mesh* mesh = nullptr; unsigned reference = ~0u;
    for (const auto& draw : r.draws) {
        const auto& p = *draw.pipeline;
        const bool changed = &p != bound;
        if (changed) {
            glUseProgram(p.program); glBindVertexArray(p.array);
            if (p.cull) { glEnable(GL_CULL_FACE); glCullFace(p.cull == 1 ? GL_FRONT : GL_BACK); } else glDisable(GL_CULL_FACE);
            if (p.blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glBlendFuncSeparate(p.blend[0], p.blend[1], p.blend[2], p.blend[3]); glBlendEquationSeparate(p.blend[4], p.blend[5]);
            glColorMask(!!(p.writeMask & 8), !!(p.writeMask & 4), !!(p.writeMask & 2), !!(p.writeMask & 1));
            glDepthFunc(p.depthCompare); glDepthMask(p.depthWrite);
            if (p.stencilEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        }
        if (changed || draw.stencilReference != reference) {
            for (const auto& [face, state] : {std::pair{GL_FRONT, &p.front}, std::pair{GL_BACK, &p.back}}) {
                glStencilFuncSeparate(face, comparison(state->compare), GLint(draw.stencilReference), state->readMask);
                glStencilOpSeparate(face, stencilOperation(state->fail), stencilOperation(state->depthFail), stencilOperation(state->pass));
                glStencilMaskSeparate(face, state->writeMask);
            }
            reference = draw.stencilReference;
        }
        if (changed || draw.mesh != mesh) {
            glVertexArrayVertexBuffer(p.array, 0, draw.mesh->vertices, 0, GLsizei(draw.mesh->stride));
            glVertexArrayElementBuffer(p.array, draw.mesh->indices);
            mesh = draw.mesh;
        }
        bound = &p;
        for (const auto& c : draw.constants) glBindBufferBase(GL_UNIFORM_BUFFER, c.binding, c.value->buffer);
        for (const auto& image : draw.images) { glBindTextureUnit(image.unit, image.value->name); glBindSampler(image.unit, image.sampler); }
        glDrawElements(GL_TRIANGLES, GLsizei(draw.indexCount), GL_UNSIGNED_INT, reinterpret_cast<const void*>(std::uintptr_t(draw.firstIndex) * 4));
    }
    // The state the native surface passes expect.
    glDisable(GL_FRAMEBUFFER_SRGB); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE); glStencilMask(0xff); glBlendEquation(GL_FUNC_ADD); glFrontFace(GL_CCW);
    glCopyImageSubData(r.color, GL_TEXTURE_2D, 0, 0, 0, 0, output, GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);
    ++r.counters.frames;
}
bool SourceGraphics::active() const noexcept { return !impl_->draws.empty(); }
// OpenGL keeps resources across idle periods, so suspension only blocks use.
void SourceGraphics::suspend() { auto& r = *impl_; r.thread(); if (r.suspended) return; (void)releaseTargets(); r.suspended = true; }
void SourceGraphics::resume(void*, void*) { auto& r = *impl_; r.thread(); r.suspended = false; }
bool SourceGraphics::suspended() const noexcept { return impl_->suspended; }
std::size_t SourceGraphics::suspendedBytes() const noexcept { return 0; }
void SourceGraphics::clear() { auto& r = *impl_; r.thread(); r.releaseObjects(); }
SourceGraphicsStats SourceGraphics::stats() const {
    auto result = impl_->counters; result.meshes = impl_->meshes.size(); result.textures = impl_->textures.size();
    result.pipelines = impl_->pipelines.size(); result.uniforms = impl_->uniforms.size(); result.draws = impl_->draws.size(); return result;
}
} // namespace endfield::native
