#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

// Controllable at runtime via: screenshader.sh --set u_curvature 0.1
uniform float u_curvature;

const float PI = 3.14159265359;

// --- Barrel distortion (CRT curvature) ---
vec2 barrel_distort(vec2 uv, float amount) {
    vec2 cc = uv - 0.5;
    float r2 = dot(cc, cc);
    return uv + cc * r2 * amount;
}

// --- Phosphor dot mask (RGB triads) ---
vec3 phosphor_mask(vec2 uv) {
    int px = int(uv.x * u_resolution.x) % 3;
    if (px == 0) return vec3(1.0, 0.7, 0.7);
    if (px == 1) return vec3(0.7, 1.0, 0.7);
    return vec3(0.7, 0.7, 1.0);
}

void main() {
    vec2 uv = v_texcoord;

    // Barrel distortion (controllable: default 0.04, 0=flat, 0.3=heavy)
    // Higher values shift visuals away from real click positions
    float curvature = u_curvature > 0.001 ? u_curvature : 0.04;
    uv = barrel_distort(uv, curvature);

    // Discard pixels outside the curved screen area
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Chromatic aberration (stronger toward edges)
    const float CHROMA_BASE = 0.0015;
    float edge_dist = length(uv - 0.5);
    float chroma_offset = CHROMA_BASE * edge_dist;
    float r = texture(u_screen, uv + vec2(chroma_offset, 0.0)).r;
    float g = texture(u_screen, uv).g;
    float b = texture(u_screen, uv - vec2(chroma_offset, 0.0)).b;
    vec3 color = vec3(r, g, b);

    // Scanlines
    const float SCANLINE_INTENSITY = 0.12;
    const float SCANLINE_WEIGHT = 1.5;
    float scanline = sin(uv.y * u_resolution.y * PI) * 0.5 + 0.5;
    scanline = pow(scanline, SCANLINE_WEIGHT);
    color *= mix(1.0, scanline, SCANLINE_INTENSITY);

    // Phosphor dot mask
    color *= phosphor_mask(uv);

    // Vignette
    const float VIGNETTE_AMOUNT = 0.4;
    vec2 vig_uv = uv * (1.0 - uv);
    float vignette = vig_uv.x * vig_uv.y * 15.0;
    vignette = clamp(pow(vignette, 0.25), 0.0, 1.0);
    color *= mix(1.0, vignette, VIGNETTE_AMOUNT);

    // Subtle flicker
    const float FLICKER_AMOUNT = 0.008;
    float flicker = 1.0 + FLICKER_AMOUNT * sin(u_time * 120.0 * PI);
    color *= flicker;

    // Brightness boost (compensate for scanline/mask darkening)
    color *= 1.2;

    frag_color = vec4(color, 1.0);
}
