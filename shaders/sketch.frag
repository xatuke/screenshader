#version 330 core

// Pencil sketch / crosshatch effect:
// Edge detection + crosshatch pattern based on luminance

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

const float PI = 3.14159265359;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Crosshatch pattern at given angle
float hatch(vec2 uv, float angle, float density) {
    float c = cos(angle);
    float s = sin(angle);
    vec2 rotated = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    return smoothstep(0.3, 0.5, abs(sin(rotated.x * density)));
}

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
    vec3 orig = texture(u_screen, uv).rgb;
    vec2 pixel = uv * u_resolution;

    // Sample original color and get luminance
    vec3 screen = texture(u_screen, uv).rgb;
    float lum = dot(screen, vec3(0.2126, 0.7152, 0.0722));
    float darkness = 1.0 - lum;

    // --- Sobel edge detection ---
    float tl = dot(texture(u_screen, uv + vec2(-px.x, -px.y)).rgb, vec3(0.333));
    float t  = dot(texture(u_screen, uv + vec2(0.0, -px.y)).rgb, vec3(0.333));
    float tr = dot(texture(u_screen, uv + vec2(px.x, -px.y)).rgb, vec3(0.333));
    float l  = dot(texture(u_screen, uv + vec2(-px.x, 0.0)).rgb, vec3(0.333));
    float r  = dot(texture(u_screen, uv + vec2(px.x, 0.0)).rgb, vec3(0.333));
    float bl = dot(texture(u_screen, uv + vec2(-px.x, px.y)).rgb, vec3(0.333));
    float b  = dot(texture(u_screen, uv + vec2(0.0, px.y)).rgb, vec3(0.333));
    float br = dot(texture(u_screen, uv + vec2(px.x, px.y)).rgb, vec3(0.333));

    float gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
    float gy = -tl - 2.0*t - tr + bl + 2.0*b + br;
    float edge = sqrt(gx*gx + gy*gy);
    edge = smoothstep(0.05, 0.35, edge);

    // --- Crosshatch shading ---
    // Layer 1: light shading (45 degrees)
    float h1 = hatch(pixel, PI / 4.0, 0.8);
    // Layer 2: medium shading (135 degrees)
    float h2 = hatch(pixel, 3.0 * PI / 4.0, 0.9);
    // Layer 3: heavy shading (near-horizontal)
    float h3 = hatch(pixel, PI / 8.0, 1.1);

    // Apply layers progressively with darkness
    float shading = 1.0;
    shading -= h1 * smoothstep(0.2, 0.5, darkness) * 0.35;
    shading -= h2 * smoothstep(0.45, 0.7, darkness) * 0.3;
    shading -= h3 * smoothstep(0.7, 0.9, darkness) * 0.25;

    // Paper color (warm off-white)
    vec3 paper = vec3(0.96, 0.94, 0.89);
    // Pencil color (dark gray, not pure black)
    vec3 pencil = vec3(0.15, 0.13, 0.12);

    // Compose: paper with pencil marks
    vec3 color = mix(paper, pencil, (1.0 - shading));

    // Add edge outlines
    color = mix(color, pencil, edge * 0.8);

    // Subtle paper grain
    float grain = hash(pixel * 0.3 + u_time * 0.01) * 0.03;
    color += grain - 0.015;

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, orig, detail * 0.8);

    frag_color = vec4(color, 1.0);
}
