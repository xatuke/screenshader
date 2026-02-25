#version 330 core

// Minecraft: blocky pixels, dirt-palette color quantization,
// block grid outlines, subtle ambient occlusion, and a dither pattern

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

// Find nearest Minecraft palette color
vec3 quantize(vec3 color) {
    vec3 best = vec3(0.0);
    float best_dist = 999.0;
    vec3 p; float d;

    p = vec3(0.53, 0.38, 0.25); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // dirt
    p = vec3(0.36, 0.25, 0.16); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // dark dirt
    p = vec3(0.37, 0.60, 0.22); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // grass
    p = vec3(0.24, 0.46, 0.14); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // dark grass
    p = vec3(0.50, 0.50, 0.50); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // stone
    p = vec3(0.35, 0.35, 0.35); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // dark stone
    p = vec3(0.62, 0.47, 0.28); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // planks
    p = vec3(0.44, 0.33, 0.18); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // dark wood
    p = vec3(0.22, 0.40, 0.72); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // water
    p = vec3(0.15, 0.30, 0.55); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // deep water
    p = vec3(0.87, 0.82, 0.55); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // sand
    p = vec3(0.20, 0.52, 0.18); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // leaves
    p = vec3(0.72, 0.72, 0.72); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // light gray
    p = vec3(0.14, 0.14, 0.14); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // obsidian
    p = vec3(0.85, 0.85, 0.85); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // snow
    p = vec3(0.76, 0.38, 0.15); d = dot(color-p, color-p); if(d < best_dist){best_dist=d; best=p;} // terracotta

    return best;
}

// Ordered 4x4 Bayer dither matrix
float bayer4(vec2 pos) {
    int x = int(mod(pos.x, 4.0));
    int y = int(mod(pos.y, 4.0));
    int index = x + y * 4;
    // Flattened 4x4 Bayer matrix / 16
    if (index ==  0) return  0.0 / 16.0;
    if (index ==  1) return  8.0 / 16.0;
    if (index ==  2) return  2.0 / 16.0;
    if (index ==  3) return 10.0 / 16.0;
    if (index ==  4) return 12.0 / 16.0;
    if (index ==  5) return  4.0 / 16.0;
    if (index ==  6) return 14.0 / 16.0;
    if (index ==  7) return  6.0 / 16.0;
    if (index ==  8) return  3.0 / 16.0;
    if (index ==  9) return 11.0 / 16.0;
    if (index == 10) return  1.0 / 16.0;
    if (index == 11) return  9.0 / 16.0;
    if (index == 12) return 15.0 / 16.0;
    if (index == 13) return  7.0 / 16.0;
    if (index == 14) return 13.0 / 16.0;
    return 5.0 / 16.0;
}

void main() {
    vec2 texel = 1.0 / u_resolution;
    vec3 orig = texture(u_screen, v_texcoord).rgb;

    // --- Pixelation ---
    float block_size = 8.0;
    vec2 blocks = u_resolution / block_size;
    vec2 block_uv = floor(v_texcoord * blocks) / blocks;
    block_uv += 0.5 / blocks;

    vec3 color = texture(u_screen, block_uv).rgb;

    // --- Boost saturation for game-like pop ---
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color = mix(vec3(luma), color, 1.4);

    // --- Dither before quantization for smoother banding ---
    vec2 pixel_pos = v_texcoord * u_resolution / block_size;
    float dither = (bayer4(pixel_pos * block_size) - 0.5) * 0.12;
    color += dither;
    color = clamp(color, 0.0, 1.0);

    // --- Quantize to Minecraft palette ---
    color = quantize(color);

    // --- Block grid lines (dark outlines between pixels) ---
    vec2 local = fract(v_texcoord * blocks);
    float edge_x = smoothstep(0.0, 0.08, local.x) * smoothstep(0.0, 0.08, 1.0 - local.x);
    float edge_y = smoothstep(0.0, 0.08, local.y) * smoothstep(0.0, 0.08, 1.0 - local.y);
    float grid = edge_x * edge_y;
    color *= mix(0.65, 1.0, grid);

    // --- Fake ambient occlusion: darken block edges slightly ---
    float ao = smoothstep(0.0, 0.25, local.x) * smoothstep(0.0, 0.25, local.y);
    color *= mix(0.85, 1.0, ao);

    // --- Subtle texture noise within each block (stone-like grain) ---
    float grain = fract(sin(dot(floor(v_texcoord * u_resolution / 2.0), vec2(12.9898, 78.233))) * 43758.5);
    color *= 0.95 + grain * 0.1;

    // --- Slight warm tint (torch-lit feel) ---
    color *= vec3(1.04, 1.0, 0.93);

    // --- Text preservation: keep original in high-detail areas ---
    float detail = textDetect(v_texcoord, texel);
    color = mix(color, orig, detail * 0.85);

    frag_color = vec4(color, 1.0);
}
