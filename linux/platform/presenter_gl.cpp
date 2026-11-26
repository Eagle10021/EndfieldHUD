#include "presenter_gl.hpp"

#include <epoxy/gl.h>

#include <stdexcept>
#include <string>

namespace endfield::linux_port {
namespace {
constexpr const char* vertex = R"(#version 450 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
})";
// The backdrop follows the Windows desktop backdrop: a near-black tint at the chosen darkness and a
// radial vignette (centre .5/.46, edge 1/1; alpha 0, .06, .38), faded together.
constexpr const char* fragment = R"(#version 450 core
layout(binding = 0) uniform sampler2D frame;
layout(location = 0) uniform ivec2 size;
layout(location = 1) uniform float opacity;
layout(location = 2) uniform float darkness;
layout(location = 0) out vec4 color;
void main() {
    ivec2 at = ivec2(int(gl_FragCoord.x), size.y - 1 - int(gl_FragCoord.y));
    vec4 hud = texelFetch(frame, at, 0);
    vec2 unit = (vec2(at) + 0.5) / vec2(size);
    float r = clamp(length((unit - vec2(0.5, 0.46)) / vec2(0.5, 0.54)), 0.0, 1.0);
    float edge = r < 0.5 ? mix(0.0, 0.06, r * 2.0) : mix(0.06, 0.38, r * 2.0 - 1.0);
    float alpha = edge + (1.0 - edge) * darkness;
    vec3 tint = vec3(0.015) * darkness * (1.0 - edge);
    vec4 backdrop = vec4(tint, alpha) * opacity;
    color = hud + backdrop * (1.0 - hud.a);
})";
GLuint compile(GLenum stage, const char* source) {
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); throw std::runtime_error(std::string("Compile presentation shader: ") + log); }
    return shader;
}
} // namespace

void Presenter::release() noexcept {
    if (program_) glDeleteProgram(program_);
    if (array_) glDeleteVertexArrays(1, &array_);
    program_ = array_ = 0;
}
void Presenter::draw(unsigned framebuffer, unsigned texture, unsigned width, unsigned height, Backdrop backdrop) {
    if (!program_) {
        const GLuint v = compile(GL_VERTEX_SHADER, vertex), f = compile(GL_FRAGMENT_SHADER, fragment);
        program_ = glCreateProgram(); glAttachShader(program_, v); glAttachShader(program_, f); glLinkProgram(program_);
        glDeleteShader(v); glDeleteShader(f);
        GLint ok{}; glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) throw std::runtime_error("Link presentation shader");
        glCreateVertexArrays(1, &array_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer); glViewport(0, 0, GLsizei(width), GLsizei(height));
    glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(program_); glBindVertexArray(array_);
    glBindTextureUnit(0, texture); glBindSampler(0, 0);
    glUniform2i(0, GLint(width), GLint(height));
    glUniform1f(1, float(backdrop.opacity)); glUniform1f(2, float(backdrop.darkness));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
} // namespace endfield::linux_port
