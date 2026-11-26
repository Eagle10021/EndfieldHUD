#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(std140, binding = 0) uniform Camera { mat4 viewProjection; };
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 linearColor;
layout(location = 0) out vec4 worldPosition;
layout(location = 1) out vec2 texturePoint;
layout(location = 2) out vec4 vertexColor;
void main() {
    worldPosition = world * vec4(position, 1.0);
    gl_Position = viewProjection * worldPosition;
    texturePoint = uv;
    vertexColor = linearColor;
}
