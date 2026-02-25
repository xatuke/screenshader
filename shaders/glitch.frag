#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash1(float p) {
    return fract(sin(p * 127.1) * 43758.5453);
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
    vec3 orig = texture(u_screen, v_texcoord).rgb;
    float t = floor(u_time * 12.0); // quantize time for glitch steps

    // --- Block displacement ---
    // Random horizontal bands that shift sideways
    float block_y = floor(uv.y * 30.0);
    float block_rand = hash(vec2(block_y, t));
    float block_active = step(0.85, block_rand); // only some blocks glitch
    float shift = (hash(vec2(block_y + 0.5, t)) - 0.5) * 0.08 * block_active;
    uv.x += shift;

    // --- RGB channel splitting ---
    float split_amount = 0.004 + 0.006 * step(0.9, hash1(t * 3.7));
    float r = texture(u_screen, uv + vec2(split_amount, 0.0)).r;
    float g = texture(u_screen, uv).g;
    float b = texture(u_screen, uv - vec2(split_amount, 0.0)).b;
    vec3 color = vec3(r, g, b);

    // --- Scanline jitter ---
    float line = floor(uv.y * u_resolution.y);
    float jitter = (hash(vec2(line, t * 2.0)) - 0.5) * 0.002;
    float jitter_active = step(0.93, hash(vec2(line + 100.0, t)));
    color = mix(color, texture(u_screen, uv + vec2(jitter, 0.0)).rgb, jitter_active);

    // --- Static noise blocks ---
    float noise_block = step(0.97, hash(vec2(
        floor(uv.x * 20.0),
        floor(uv.y * 20.0) + t
    )));
    color = mix(color, vec3(hash(uv * u_resolution + t)), noise_block * 0.6);

    // --- Color quantization (occasional) ---
    float quantize_active = step(0.92, hash1(t * 1.3));
    if (quantize_active > 0.5) {
        color = floor(color * 6.0) / 6.0;
    }

    // --- Brief white flash ---
    float flash = step(0.98, hash1(t * 7.7)) * 0.15;
    color += flash;

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(v_texcoord, texel);
    color = mix(color, orig, detail * 0.75);

    frag_color = vec4(color, 1.0);
}
