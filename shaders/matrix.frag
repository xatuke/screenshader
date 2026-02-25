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
    vec3 screen = texture(u_screen, uv).rgb;

    // Convert screen to green-tinted luminance
    float lum = dot(screen, vec3(0.2126, 0.7152, 0.0722));
    vec3 color = vec3(0.0, lum, 0.0);

    // Rain columns
    float cell_size = 16.0;
    vec2 cell = floor(uv * u_resolution / cell_size);
    float col_speed = hash(vec2(cell.x, 0.0)) * 2.0 + 1.0;
    float col_offset = hash(vec2(cell.x, 1.0)) * 100.0;

    // Falling position within this column
    float fall = fract((u_time * col_speed + col_offset) * 0.3 - cell.y * 0.05);

    // Rain trail (bright head, fading tail)
    float trail = smoothstep(0.0, 0.6, fall) * smoothstep(1.0, 0.6, fall);
    trail = pow(trail, 2.0);

    // Bright head
    float head = smoothstep(0.85, 1.0, fall) * 1.5;

    // Random flicker per cell
    float flicker = hash(cell + floor(u_time * 10.0)) * 0.3 + 0.7;

    // Compose rain effect over screen
    float rain = (trail * 0.3 + head * 0.8) * flicker;
    color += vec3(0.0, rain * 0.6, rain * 0.1);

    // Boost greens from original screen content
    color.g += lum * 0.5;

    // Scanlines
    float scanline = sin(uv.y * u_resolution.y * 3.14159) * 0.5 + 0.5;
    color *= mix(1.0, scanline, 0.06);

    // Slight vignette
    vec2 vig = uv * (1.0 - uv);
    color *= clamp(pow(vig.x * vig.y * 15.0, 0.3), 0.0, 1.0);

    // Text preservation: readable green monochrome without rain in text areas
    float detail = textDetect(uv, texel);
    vec3 text_readable = vec3(0.05, lum * 1.3, 0.03);
    color = mix(color, text_readable, detail * 0.7);

    frag_color = vec4(color, 1.0);
}
