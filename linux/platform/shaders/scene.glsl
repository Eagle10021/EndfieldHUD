// OpenGL port of windows/native/hud.hlsl (scene pass). The uniform blocks match the C++
// ObjectUniform layout (std140, column-major matrices). Variants are selected with
// ENCODED_INPUT, ENCODED_OUTPUT and ALPHA_INPUT.
struct PlaneMask {
    mat4 worldToLocal;
    vec4 bounds;
    vec4 corners;
};
layout(std140, binding = 1) uniform Object {
    mat4 world;
    vec4 linearTint;
    float opacity;
    uint maskCount;
    uint shutterEnabled;
    uint shutterStrips;
    PlaneMask masks[8];
    mat4 shutterWorldToLocal;
    vec4 shutterEdges[30];
    mat4 alphaWorldToLocal;
    vec4 alphaBounds;
    vec4 alphaControl;
    mat4 angularWorldToLocal;
    vec4 angularCenterControl;
    vec4 angularNormals;
};
