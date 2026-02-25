#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

const float PI = 3.14159265359;

// Simple hash for noise
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
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

    // Horizontal jitter (per-scanline random offset)
    float line = floor(uv.y * u_resolution.y);
    float jitter_seed = hash(vec2(line, floor(u_time * 30.0)));
    float jitter = (jitter_seed - 0.5) * 0.003;
    uv.x += jitter;

    // Color bleeding (horizontal blur weighted toward red)
    float px = 1.0 / u_resolution.x;
    float r = texture(u_screen, uv + vec2(px * 2.0, 0.0)).r * 0.3
            + texture(u_screen, uv + vec2(px, 0.0)).r * 0.3
            + texture(u_screen, uv).r * 0.4;
    float g = texture(u_screen, uv + vec2(px, 0.0)).g * 0.2
            + texture(u_screen, uv).g * 0.6
            + texture(u_screen, uv - vec2(px, 0.0)).g * 0.2;
    float b = texture(u_screen, uv).b * 0.4
            + texture(u_screen, uv - vec2(px, 0.0)).b * 0.3
            + texture(u_screen, uv - vec2(px * 2.0, 0.0)).b * 0.3;
    vec3 color = vec3(r, g, b);

    // Tracking lines (bright horizontal bands that scroll)
    float track_pos = fract(u_time * 0.05);
    float track_dist = abs(uv.y - track_pos);
    float tracking = smoothstep(0.02, 0.0, track_dist) * 0.15;
    color += tracking;

    // Second tracking line
    float track_pos2 = fract(u_time * 0.08 + 0.4);
    float track_dist2 = abs(uv.y - track_pos2);
    float tracking2 = smoothstep(0.01, 0.0, track_dist2) * 0.1;
    color += tracking2;

    // Noise overlay
    float noise = hash(uv * u_resolution + u_time * 100.0);
    color += (noise - 0.5) * 0.04;

    // Slight desaturation
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, 0.75);

    // Slight vignette
    vec2 vig = uv * (1.0 - uv);
    float vignette = vig.x * vig.y * 15.0;
    vignette = clamp(pow(vignette, 0.3), 0.0, 1.0);
    color *= mix(1.0, vignette, 0.3);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(v_texcoord, texel);
    color = mix(color, orig, detail * 0.6);

    frag_color = vec4(color, 1.0);
}
