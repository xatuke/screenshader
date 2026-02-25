#version 330 core

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
    vec2 texel = 1.0 / u_resolution;
    vec3 orig = texture(u_screen, v_texcoord).rgb;

    // Grid size in pixels
    float dot_size = 6.0;
    vec2 pixel = v_texcoord * u_resolution;

    // Rotate grid 45 degrees for classic halftone look
    float angle = PI / 4.0;
    mat2 rot = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    vec2 rotated = rot * pixel;

    // Cell coordinates
    vec2 cell = floor(rotated / dot_size);
    vec2 cell_center = (cell + 0.5) * dot_size;
    vec2 cell_uv = (inverse(rot) * cell_center) / u_resolution;

    // Sample screen color at cell center
    vec3 screen = texture(u_screen, clamp(cell_uv, 0.0, 1.0)).rgb;
    float lum = dot(screen, vec3(0.2126, 0.7152, 0.0722));

    // Distance from pixel to cell center (in rotated space)
    vec2 local = fract(rotated / dot_size) - 0.5;
    float dist = length(local);

    // Dot radius scales with luminance (brighter = bigger dot)
    float radius = lum * 0.48;

    // Anti-aliased dot
    float dot_mask = 1.0 - smoothstep(radius - 0.04, radius + 0.04, dist);

    // Paper background with slight warmth
    vec3 paper = vec3(0.95, 0.93, 0.88);
    vec3 ink = screen * 0.8;

    vec3 color = mix(paper, ink, dot_mask);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(v_texcoord, texel);
    color = mix(color, orig, detail * 0.8);

    frag_color = vec4(color, 1.0);
}
