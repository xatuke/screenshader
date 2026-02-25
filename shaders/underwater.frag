#version 330 core

// Underwater / aquatic effect:
// Caustic light patterns, blue-green tint, wave distortion, light rays

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

const float PI = 3.14159265359;

// Simple caustic pattern
float caustic(vec2 uv, float t) {
    float c = 0.0;
    c += sin(uv.x * 15.0 + t * 1.2) * sin(uv.y * 12.0 + t * 0.8);
    c += sin(uv.x * 8.0 - t * 0.9 + uv.y * 10.0) * 0.5;
    c += sin((uv.x + uv.y) * 20.0 + t * 1.5) * 0.3;
    return c * 0.5 + 0.5;
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
    vec2 texel = 1.0 / u_resolution;
    vec3 orig = texture(u_screen, uv).rgb;

    // --- Wave distortion ---
    float wave_x = sin(uv.y * 12.0 + u_time * 1.5) * 0.003;
    float wave_y = cos(uv.x * 10.0 + u_time * 1.2) * 0.002;
    vec2 distorted_uv = uv + vec2(wave_x, wave_y);

    vec3 color = texture(u_screen, distorted_uv).rgb;

    // --- Blue-green tint (deeper = more tint) ---
    // Simulate depth: top of screen is surface, bottom is deep
    float depth = uv.y;
    vec3 water_tint = vec3(0.1, 0.4, 0.5);
    color = mix(color, color * water_tint * 2.0, 0.2 + depth * 0.3);

    // Reduce reds with depth (red light absorbs first underwater)
    color.r *= 1.0 - depth * 0.35;

    // --- Caustic light patterns ---
    float c = caustic(uv, u_time);
    float caustic_intensity = c * (1.0 - depth * 0.7) * 0.15;
    color += vec3(0.2, 0.6, 0.5) * caustic_intensity;

    // --- Light rays from surface ---
    float ray_x = sin(uv.x * 5.0 + u_time * 0.3);
    float ray = pow(max(ray_x, 0.0), 8.0) * (1.0 - uv.y) * 0.08;
    color += vec3(0.3, 0.7, 0.6) * ray;

    // --- Particle / bubble hints ---
    vec2 bubble_uv = uv * vec2(30.0, 20.0);
    float bubble_t = fract(-u_time * 0.3 + floor(bubble_uv.x) * 0.37);
    vec2 bubble_cell = fract(bubble_uv) - 0.5;
    bubble_cell.y -= bubble_t;
    float bubble = smoothstep(0.08, 0.04, length(bubble_cell));
    float bubble_vis = step(0.9, fract(sin(floor(bubble_uv.x) * 127.1) * 43758.5));
    color += vec3(0.3, 0.6, 0.7) * bubble * bubble_vis * 0.15;

    // --- Slight vignette ---
    vec2 vig = uv * (1.0 - uv);
    float vignette = clamp(pow(vig.x * vig.y * 15.0, 0.3), 0.0, 1.0);
    color *= mix(0.6, 1.0, vignette);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, texel);
    color = mix(color, orig, detail * 0.65);

    frag_color = vec4(color, 1.0);
}
