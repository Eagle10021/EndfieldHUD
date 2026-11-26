#version 450 core
// Linear premultiplied composition to encoded premultiplied output (CompositePS in hud.hlsl).
layout(binding = 0) uniform sampler2D linearComposition;
layout(location = 0) out vec4 outColor;
vec3 encodeSRGB(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(lessThanEqual(c, vec3(0.0031308))));
}
void main() {
    vec4 value = texelFetch(linearComposition, ivec2(gl_FragCoord.xy), 0);
    float alpha = clamp(value.a, 0.0, 1.0);
    outColor = alpha <= 0.0 ? vec4(0.0) : vec4(encodeSRGB(value.rgb / alpha) * alpha, alpha);
}
