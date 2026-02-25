#version 330 core

// Rain overlay: animated falling raindrops, water streaks, and slight fog

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

// Hash functions for pseudo-random values
float hash(float n) {
    return fract(sin(n) * 43758.5453);
}

float hash2(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Smooth noise
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash2(i);
    float b = hash2(i + vec2(1.0, 0.0));
    float c = hash2(i + vec2(0.0, 1.0));
    float d = hash2(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Single raindrop streak — returns intensity
float raindrop(vec2 uv, float seed, float time) {
    float x_pos = hash(seed * 1.73);
    float speed = 0.6 + hash(seed * 3.17) * 0.8;
    float len = 0.02 + hash(seed * 7.13) * 0.04;
    float width = 0.0008 + hash(seed * 2.31) * 0.0006;

    // Horizontal position with slight wind sway
    float wind = sin(time * 0.5 + seed) * 0.02;
    float x = x_pos + wind;

    // Vertical: falling with time, wrapping
    float y = fract(time * speed + hash(seed * 5.91));

    // Distance from the streak line
    float dx = abs(uv.x - x);
    float dy = uv.y - y;

    // Streak: trail above the drop head (positive dy = above in screen space)
    float streak = 1.0 - smoothstep(0.0, len, dy);
    streak *= step(0.0, dy);
    streak *= 1.0 - smoothstep(0.0, width, dx);

    // Bright drop head
    float head = smoothstep(0.003, 0.0, length(uv - vec2(x, y)));

    return (streak * 0.4 + head * 0.8);
}

// Layer of many raindrops
float rain_layer(vec2 uv, float layer_seed, int count, float time) {
    float r = 0.0;
    for (int i = 0; i < count; i++) {
        r += raindrop(uv, float(i) * 1.7 + layer_seed, time);
    }
    return r;
}

// Water droplet on "glass" — circular lens distortion
vec2 droplet_offset(vec2 uv, vec2 center, float radius, float strength) {
    vec2 d = uv - center;
    float dist = length(d);
    if (dist < radius) {
        float t = 1.0 - dist / radius;
        t = t * t;
        return d * t * strength;
    }
    return vec2(0.0);
}

void main() {
    vec2 uv = v_texcoord;
    vec2 texel = 1.0 / u_resolution;

    // --- Glass droplets: stationary water beads that distort ---
    vec2 offset = vec2(0.0);
    for (int i = 0; i < 12; i++) {
        float seed = float(i);
        vec2 center = vec2(hash(seed * 3.1), hash(seed * 7.9));
        // Slowly drift downward
        center.y = fract(center.y - u_time * 0.01 * (0.5 + hash(seed * 2.3)));
        float radius = 0.008 + hash(seed * 5.3) * 0.015;
        offset += droplet_offset(uv, center, radius, 0.015);
    }

    vec3 color = texture(u_screen, uv + offset).rgb;

    // --- Falling rain streaks (two layers for depth) ---
    float rain = 0.0;
    rain += rain_layer(uv, 0.0, 40, u_time) * 0.7;   // foreground: brighter, fewer
    rain += rain_layer(uv, 100.0, 60, u_time) * 0.3;  // background: dimmer, more

    // Rain color: pale blue-white
    vec3 rain_color = vec3(0.7, 0.75, 0.85);
    color += rain_color * rain * 0.35;

    // --- Slight desaturation and cool tint (overcast mood) ---
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(color, vec3(luma), 0.15);
    color *= vec3(0.92, 0.95, 1.0);

    // --- Fog / mist at bottom of screen ---
    float fog = smoothstep(0.8, 1.0, uv.y) * 0.08;
    float fog_noise = noise(uv * 8.0 + vec2(u_time * 0.2, 0.0));
    color = mix(color, vec3(0.6, 0.65, 0.72), fog * fog_noise);

    // --- Subtle darkening (overcast sky) ---
    color *= 0.92;

    // --- Vignette ---
    vec2 vig = uv * (1.0 - uv);
    float vignette = clamp(pow(vig.x * vig.y * 15.0, 0.35), 0.0, 1.0);
    color *= mix(0.7, 1.0, vignette);

    frag_color = vec4(color, 1.0);
}
