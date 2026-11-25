#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(push_constant) uniform Present {
    uint encodeSrgb;
    uint transform;
} present;
void main() {
    vec2 coordinate = uv;
    if (present.transform == 2u || present.transform == 32u) coordinate = vec2(uv.y, 1.0 - uv.x);
    else if (present.transform == 4u || present.transform == 64u) coordinate = 1.0 - uv;
    else if (present.transform == 8u || present.transform == 128u) coordinate = vec2(1.0 - uv.y, uv.x);
    if ((present.transform & 240u) != 0u) coordinate.x = 1.0 - coordinate.x;
    color = texture(sourceImage, coordinate);
    if (present.encodeSrgb != 0u) {
        vec3 linear = clamp(color.rgb, 0.0, 1.0);
        color.rgb = mix(12.92 * linear, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055,
            greaterThan(linear, vec3(0.0031308)));
    }
}
