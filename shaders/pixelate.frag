#version 330 core

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
    vec2 texel = 1.0 / u_resolution;
    vec3 orig = texture(u_screen, v_texcoord).rgb;

    // Pixel block size (bigger = more pixelated, like Minecraft)
    float block_size = 8.0;
    vec2 blocks = u_resolution / block_size;

    // Snap to grid
    vec2 block_uv = floor(v_texcoord * blocks) / blocks;
    // Center sample within block
    block_uv += 0.5 / blocks;

    vec3 color = texture(u_screen, block_uv).rgb;

    // Slightly boost saturation for that game-like pop
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, 1.3);

    // Subtle grid lines between pixels
    vec2 local = fract(v_texcoord * blocks);
    float grid = smoothstep(0.0, 0.06, local.x) * smoothstep(0.0, 0.06, local.y);
    color *= mix(0.85, 1.0, grid);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(v_texcoord, texel);
    color = mix(color, orig, detail * 0.85);

    frag_color = vec4(color, 1.0);
}
