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

    // Green phosphor tint
    vec3 green = vec3(0.0, 1.0, 0.3) * lum;

    // Phosphor glow (sample neighbors for soft bloom)
    float px = 1.0 / u_resolution.x;
    float py = 1.0 / u_resolution.y;
    float bloom = 0.0;
    bloom += dot(texture(u_screen, uv + vec2(-px, 0.0)).rgb, vec3(0.2126, 0.7152, 0.0722));
    bloom += dot(texture(u_screen, uv + vec2( px, 0.0)).rgb, vec3(0.2126, 0.7152, 0.0722));
    bloom += dot(texture(u_screen, uv + vec2(0.0, -py)).rgb, vec3(0.2126, 0.7152, 0.0722));
    bloom += dot(texture(u_screen, uv + vec2(0.0,  py)).rgb, vec3(0.2126, 0.7152, 0.0722));
    bloom *= 0.25;
    green += vec3(0.0, 0.15, 0.05) * bloom;

    // Stronger scanlines
    float scanline = sin(uv.y * u_resolution.y * PI) * 0.5 + 0.5;
    scanline = pow(scanline, 1.8);
    green *= mix(1.0, scanline, 0.18);

    // Vignette
    vec2 vig = uv * (1.0 - uv);
    float vignette = vig.x * vig.y * 15.0;
    vignette = clamp(pow(vignette, 0.3), 0.0, 1.0);
    green *= mix(1.0, vignette, 0.4);

    // Brightness boost
    green *= 1.3;

    frag_color = vec4(green, 1.0);
}
