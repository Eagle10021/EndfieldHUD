#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "scene.glsl"
layout(binding = 0) uniform sampler2D colorTexture;
layout(binding = 1) uniform sampler2D alphaMaskTexture;
layout(location = 0) in vec4 worldPosition;
layout(location = 1) in vec2 texturePoint;
layout(location = 2) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;

vec3 encodeSRGB(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, vec3(lessThanEqual(c, vec3(0.0031308))));
}
vec3 decodeSRGB(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, vec3(lessThanEqual(c, vec3(0.04045))));
}
// Maps the fragment into a mask plane; fragments behind the plane are discarded.
vec2 planePoint(mat4 worldToLocal) {
    vec4 local = worldToLocal * worldPosition;
    if (local.w - 0.0000001 < 0.0) discard;
    return local.xy / local.w;
}
bool outside(vec2 p, vec4 bounds) { return any(lessThan(vec4(p - bounds.xy, bounds.zw - p), vec4(0.0))); }

void main() {
    for (uint i = 0u; i < maskCount; ++i) {
        // An excluding mask (corners.y) discards the inside of its rectangle.
        if (masks[i].corners.y > 0.0) {
            vec4 local = masks[i].worldToLocal * worldPosition;
            if (local.w > 0.0000001) {
                vec2 hole = local.xy / local.w;
                if (all(greaterThanEqual(hole, masks[i].bounds.xy)) && all(lessThanEqual(hole, masks[i].bounds.zw))) discard;
            }
            continue;
        }
        vec2 p = planePoint(masks[i].worldToLocal);
        if (outside(p, masks[i].bounds)) discard;
        float radius = masks[i].corners.x;
        if (radius > 0.0) {
            vec2 nearest = clamp(p, masks[i].bounds.xy + radius, masks[i].bounds.zw - radius);
            vec2 corner = (p - nearest) / radius;
            if (1.0 - dot(corner, corner) < 0.0) discard;
        }
    }
    if (shutterEnabled != 0u) {
        vec3 p = vec3(planePoint(shutterWorldToLocal), 1.0);
        // Mode 2: four six-vertex strips. Otherwise six pentagons.
        uint strips = shutterEnabled == 2u ? 4u : 6u, edges = shutterEnabled == 2u ? 6u : 5u;
        bool covered = false;
        for (uint strip = 0u; strip < strips; ++strip) {
            if ((shutterStrips & (1u << strip)) == 0u) continue;
            bool inside = true;
            for (uint edge = 0u; edge < edges; ++edge)
                inside = inside && dot(shutterEdges[strip * edges + edge].xyz, p) >= 0.0;
            covered = covered || inside;
        }
        if (!covered) discard;
    }
    vec4 sampled = texture(colorTexture, texturePoint);
#ifdef ALPHA_INPUT
    sampled = sampled.aaaa;   // alpha-only textures read as premultiplied white
#endif
    vec4 tint = vertexColor * linearTint;
    float opacityFactor = clamp(tint.a * opacity, 0.0, 1.0);
    if (alphaControl.x != 0.0) {
        vec2 p = planePoint(alphaWorldToLocal);
        if (outside(p, alphaBounds)) discard;
        vec2 maskPoint = (p - alphaBounds.xy) / (alphaBounds.zw - alphaBounds.xy);
        opacityFactor *= clamp(texture(alphaMaskTexture, maskPoint).a, 0.0, 1.0);
    }
    if (angularCenterControl.z != 0.0) {
        if (angularCenterControl.z == 1.0) {
            opacityFactor = 0.0;
        } else if (angularCenterControl.z != 4.0) {
            vec2 p = planePoint(angularWorldToLocal) - angularCenterControl.xy;
            vec2 distances = vec2(dot(angularNormals.xy, p), dot(angularNormals.zw, p) + angularCenterControl.w);
            vec2 coverage = clamp(distances / max(fwidth(distances), vec2(0.0000001)) + 0.5, 0.0, 1.0);
            // Overlapping butt-edge coverage is subtracted for narrow arcs.
            bool opposed = dot(angularNormals.xy, angularNormals.zw) < 0.0;
            opacityFactor *= angularCenterControl.z == 2.0
                ? (opposed ? clamp(coverage.x + coverage.y - 1.0, 0.0, 1.0) : min(coverage.x, coverage.y))
                : (opposed ? clamp(coverage.x + coverage.y, 0.0, 1.0) : max(coverage.x, coverage.y));
        }
    }
#if !defined(ENCODED_INPUT) && !defined(ENCODED_OUTPUT)
    outColor = vec4(sampled.rgb * tint.rgb * opacityFactor, sampled.a * opacityFactor);
#else
    if (sampled.a <= 0.0) { outColor = vec4(0.0); return; }
    vec3 color = sampled.rgb / sampled.a;
#if defined(ENCODED_INPUT) && defined(ENCODED_OUTPUT)
    color *= encodeSRGB(tint.rgb);
#elif defined(ENCODED_INPUT)
    color = decodeSRGB(color) * tint.rgb;
#else
    color = encodeSRGB(color * tint.rgb);
#endif
    outColor = vec4(color * sampled.a * opacityFactor, sampled.a * opacityFactor);
#endif
}
