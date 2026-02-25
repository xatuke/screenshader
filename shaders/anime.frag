#version 330 core

// Studio Ghibli / anime painterly look:
// Soft edges, watercolor bleed, warm pastel palette, gentle glow

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
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

    // --- Slight watercolor bleed (directional blur) ---
    // Sample in a small neighborhood with weighted average
    vec3 color = vec3(0.0);
    float total = 0.0;
    for (int x = -2; x <= 2; x++) {
        for (int y = -2; y <= 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * px * 1.5;
            float weight = 1.0 / (1.0 + float(x*x + y*y));
            color += texture(u_screen, uv + offset).rgb * weight;
            total += weight;
        }
    }
    color /= total;

    // --- Soft cel-shading (3 color levels, smooth transitions) ---
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float shading = smoothstep(0.0, 0.35, lum) * 0.33
                  + smoothstep(0.35, 0.65, lum) * 0.33
                  + smoothstep(0.65, 1.0, lum) * 0.34;
    color = color * (0.6 + shading * 0.6);

    // --- Warm Ghibli palette shift ---
    // Push toward warm pastels
    color.r = pow(color.r, 0.92);
    color.g = pow(color.g, 0.97);
    color.b = pow(color.b, 1.08);

    // Lift shadows with warm tint (Ghibli films have warm shadows)
    vec3 shadow_tint = vec3(0.15, 0.08, 0.12);
    color += shadow_tint * (1.0 - lum) * 0.2;

    // Boost saturation gently
    float qlum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(qlum), color, 1.2);

    // --- Soft edge outlines (thin, subtle) ---
    vec3 c  = texture(u_screen, uv).rgb;
    vec3 r  = texture(u_screen, uv + vec2(px.x, 0.0)).rgb;
    vec3 b  = texture(u_screen, uv + vec2(0.0, px.y)).rgb;
    float edge = length(c - r) + length(c - b);
    edge = smoothstep(0.08, 0.25, edge);
    color = mix(color, color * 0.4, edge * 0.35);

    // --- Gentle bloom/glow ---
    vec3 bloom = vec3(0.0);
    for (int i = 1; i <= 3; i++) {
        float r = float(i) * 2.0;
        bloom += texture(u_screen, uv + vec2(r, 0.0) * px).rgb;
        bloom += texture(u_screen, uv - vec2(r, 0.0) * px).rgb;
        bloom += texture(u_screen, uv + vec2(0.0, r) * px).rgb;
        bloom += texture(u_screen, uv - vec2(0.0, r) * px).rgb;
    }
    bloom /= 12.0;
    color += (bloom - color) * 0.08;

    // --- Paper texture (very subtle grain) ---
    float grain = (hash(uv * u_resolution * 0.5 + u_time * 0.1) - 0.5) * 0.02;
    color += grain;

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, orig, detail * 0.7);

    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
