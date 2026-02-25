#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

// Thermal palette: black -> blue -> purple -> red -> orange -> yellow -> white
vec3 thermal_palette(float t) {
    vec3 a = vec3(0.0, 0.0, 0.2);  // cold: dark blue
    vec3 b = vec3(0.3, 0.0, 0.5);  // cool: purple
    vec3 c = vec3(0.8, 0.0, 0.2);  // warm: red
    vec3 d = vec3(1.0, 0.5, 0.0);  // hot: orange
    vec3 e = vec3(1.0, 1.0, 0.3);  // very hot: yellow
    vec3 f = vec3(1.0, 1.0, 1.0);  // extreme: white

    if (t < 0.2) return mix(a, b, t / 0.2);
    if (t < 0.4) return mix(b, c, (t - 0.2) / 0.2);
    if (t < 0.6) return mix(c, d, (t - 0.4) / 0.2);
    if (t < 0.8) return mix(d, e, (t - 0.6) / 0.2);
    return mix(e, f, (t - 0.8) / 0.2);
}

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
    vec2 texel = 1.0 / u_resolution;
    vec3 screen = texture(u_screen, uv).rgb;

    // Luminance as "heat"
    float heat = dot(screen, vec3(0.2126, 0.7152, 0.0722));

    // Slight heat shimmer distortion
    float shimmer_x = sin(uv.y * 80.0 + u_time * 3.0) * 0.0008;
    float shimmer_y = cos(uv.x * 60.0 + u_time * 2.5) * 0.0005;
    vec3 shimmer_sample = texture(u_screen, uv + vec2(shimmer_x, shimmer_y)).rgb;
    heat = mix(heat, dot(shimmer_sample, vec3(0.2126, 0.7152, 0.0722)), 0.3);

    // Map to thermal palette
    vec3 color = thermal_palette(clamp(heat, 0.0, 1.0));

    // Sensor noise
    float noise = (hash(uv * u_resolution + u_time * 50.0) - 0.5) * 0.06;
    color += noise;

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, texel);
    color = mix(color, screen, detail * 0.75);

    frag_color = vec4(color, 1.0);
}
