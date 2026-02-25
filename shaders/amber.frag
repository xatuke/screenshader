#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

const float PI = 3.14159265359;

void main() {
    vec2 uv = v_texcoord;
    vec3 color = texture(u_screen, uv).rgb;

    // Convert to luminance
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Amber tint
    vec3 amber = vec3(1.0, 0.7, 0.0) * lum;

    // Subtle scanlines
    float scanline = sin(uv.y * u_resolution.y * PI) * 0.5 + 0.5;
    scanline = pow(scanline, 1.5);
    amber *= mix(1.0, scanline, 0.08);

    // Vignette
    vec2 vig = uv * (1.0 - uv);
    float vignette = vig.x * vig.y * 15.0;
    vignette = clamp(pow(vignette, 0.3), 0.0, 1.0);
    amber *= mix(1.0, vignette, 0.35);

    // Slight brightness boost
    amber *= 1.3;

    frag_color = vec4(amber, 1.0);
}
