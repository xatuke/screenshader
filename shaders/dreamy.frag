#version 330 core

// Dreamy / ethereal soft focus:
// Bloom glow, chromatic shift, soft pastel colors, light leaks

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

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
    vec3 color = orig;

    // --- Soft blur / bloom ---
    vec3 bloom = vec3(0.0);
    float weights = 0.0;
    for (int x = -4; x <= 4; x++) {
        for (int y = -4; y <= 4; y++) {
            float w = 1.0 / (1.0 + float(x*x + y*y));
            bloom += texture(u_screen, uv + vec2(float(x), float(y)) * px * 2.5).rgb * w;
            weights += w;
        }
    }
    bloom /= weights;

    // Mix bloom with original (additive for glow)
    color = mix(color, bloom, 0.35);
    color += bloom * 0.15; // additive glow

    // --- Soft chromatic shift at edges ---
    float edge_dist = length(uv - 0.5) * 0.7;
    float ca = edge_dist * 0.003;
    color.r = mix(color.r, texture(u_screen, uv + vec2(ca, 0.0)).r, 0.4);
    color.b = mix(color.b, texture(u_screen, uv - vec2(ca, 0.0)).b, 0.4);

    // --- Warm pastel color grade ---
    color = pow(color, vec3(0.95, 0.97, 1.05));
    // Lift blacks
    color = color * 0.88 + 0.08;
    // Push toward warm pastels
    color.r = color.r * 1.04 + 0.02;
    color.g = color.g * 1.01;
    color.b = color.b * 0.96 + 0.03;

    // Gentle desaturation
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, 0.8);

    // --- Light leak (warm gradient from corner) ---
    float leak_angle = u_time * 0.03;
    vec2 leak_center = vec2(0.8 + sin(leak_angle) * 0.2, 0.2 + cos(leak_angle) * 0.2);
    float leak_dist = length(uv - leak_center);
    vec3 leak_color = vec3(1.0, 0.85, 0.6);
    float leak = smoothstep(0.7, 0.0, leak_dist) * 0.12;
    color += leak_color * leak;

    // --- Soft vignette ---
    vec2 vig = uv * (1.0 - uv);
    float vignette = vig.x * vig.y * 15.0;
    vignette = clamp(pow(vignette, 0.4), 0.0, 1.0);
    color *= mix(0.7, 1.0, vignette);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, orig, detail * 0.55);

    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
