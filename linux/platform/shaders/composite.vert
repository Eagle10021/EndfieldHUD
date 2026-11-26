#version 450 core
// One triangle covering the target.
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
