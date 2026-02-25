#version 330 core

// Neon edge glow / cyberpunk wireframe:
// Dark background with glowing neon-colored edges

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

const float PI = 3.14159265359;

// Detect high-detail regions (text) via local edge density
float textDetect(vec2 tc, vec2 texel) {
    vec3 W = vec3(0.299, 0.587, 0.114);
    float c = dot(texture(u_screen, tc).rgb, W);
    float e = 0.0;
    e += abs(c - dot(texture(u_screen, tc + vec2(-texel.x, 0.0)).rgb, W));
    e += abs(c - dot(texture(u_screen, tc + vec2( texel.x, 0.0)).rgb, W));
    e += abs(c - dot(texture(u_screen, tc + vec2(0.0, -texel.y)).rgb, W));
    e += abs(c - dot(texture(u_screen, tc + vec2(0.0,  texel.y)).rgb, W));
    e += abs(c - dot(texture(u_screen, tc + vec2(-texel.x, -texel.y)).rgb, W));
    e += abs(c - dot(texture(u_screen, tc + vec2( texel.x,  texel.y)).rgb, W));
    return smoothstep(0.15, 0.6, e);
}

void main() {
    vec2 uv = v_texcoord;
    vec2 px = 1.0 / u_resolution;

    // --- Sobel edge detection ---
    float samples[9];
    int idx = 0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 s = texture(u_screen, uv + vec2(float(x), float(y)) * px).rgb;
            samples[idx++] = dot(s, vec3(0.2126, 0.7152, 0.0722));
        }
    }

    float gx = -samples[0] - 2.0*samples[3] - samples[6]
              + samples[2] + 2.0*samples[5] + samples[8];
    float gy = -samples[0] - 2.0*samples[1] - samples[2]
              + samples[6] + 2.0*samples[7] + samples[8];
    float edge = sqrt(gx*gx + gy*gy);
    float edge_sharp = smoothstep(0.05, 0.3, edge);

    // --- Edge direction for color cycling ---
    float angle = atan(gy, gx);
    float hue_shift = angle / (2.0 * PI) + 0.5 + u_time * 0.05;

    // HSV to RGB for neon rainbow edges
    float h = fract(hue_shift) * 6.0;
    float f = fract(h);
    vec3 neon;
    if (h < 1.0)      neon = vec3(1.0, f, 0.0);
    else if (h < 2.0) neon = vec3(1.0 - f, 1.0, 0.0);
    else if (h < 3.0) neon = vec3(0.0, 1.0, f);
    else if (h < 4.0) neon = vec3(0.0, 1.0 - f, 1.0);
    else if (h < 5.0) neon = vec3(f, 0.0, 1.0);
    else               neon = vec3(1.0, 0.0, 1.0 - f);

    // Boost neon brightness
    neon = neon * 1.5 + 0.2;

    // --- Glow (wider edge bloom) ---
    float glow = 0.0;
    for (int i = 1; i <= 4; i++) {
        float r = float(i) * 1.5;
        vec3 s;
        s = texture(u_screen, uv + vec2(r, 0.0) * px).rgb;
        glow += dot(s, vec3(0.2126, 0.7152, 0.0722));
        s = texture(u_screen, uv - vec2(r, 0.0) * px).rgb;
        glow += dot(s, vec3(0.2126, 0.7152, 0.0722));
        s = texture(u_screen, uv + vec2(0.0, r) * px).rgb;
        glow += dot(s, vec3(0.2126, 0.7152, 0.0722));
        s = texture(u_screen, uv - vec2(0.0, r) * px).rgb;
        glow += dot(s, vec3(0.2126, 0.7152, 0.0722));
    }
    glow /= 16.0;
    float glow_edge = smoothstep(0.03, 0.2, edge) * 0.3;

    // --- Compose: dark background + neon edges + glow ---
    vec3 screen = texture(u_screen, uv).rgb;
    float screen_lum = dot(screen, vec3(0.2126, 0.7152, 0.0722));

    // Darken the base image
    vec3 dark_base = screen * 0.12;

    // Add neon edges
    vec3 color = dark_base + neon * edge_sharp;

    // Add soft glow around edges
    color += neon * glow_edge * 0.5;

    // Keep very bright areas slightly visible
    color += screen * smoothstep(0.8, 1.0, screen_lum) * 0.3;

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, screen, detail * 0.75);

    frag_color = vec4(color, 1.0);
}
