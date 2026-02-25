#version 330 core

// Cel-shaded / Marvel animated art style:
// Flat shading bands, bold ink outlines, saturated colors

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

    // --- Sobel edge detection ---
    vec3 tl = texture(u_screen, uv + vec2(-px.x, -px.y)).rgb;
    vec3 t  = texture(u_screen, uv + vec2( 0.0,  -px.y)).rgb;
    vec3 tr = texture(u_screen, uv + vec2( px.x, -px.y)).rgb;
    vec3 l  = texture(u_screen, uv + vec2(-px.x,  0.0)).rgb;
    vec3 c  = texture(u_screen, uv).rgb;
    vec3 r  = texture(u_screen, uv + vec2( px.x,  0.0)).rgb;
    vec3 bl = texture(u_screen, uv + vec2(-px.x,  px.y)).rgb;
    vec3 b  = texture(u_screen, uv + vec2( 0.0,   px.y)).rgb;
    vec3 br = texture(u_screen, uv + vec2( px.x,  px.y)).rgb;

    vec3 gx = -tl - 2.0*l - bl + tr + 2.0*r + br;
    vec3 gy = -tl - 2.0*t - tr + bl + 2.0*b + br;
    float edge = length(gx) + length(gy);
    edge = smoothstep(0.25, 0.7, edge);

    // --- Cel-shading: flat luminance bands ---
    vec3 color = c;
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // 4 flat shading tiers with hard-ish transitions
    float shade;
    shade  = smoothstep(0.0,  0.15, lum) * 0.30;  // deep shadow → shadow
    shade += smoothstep(0.15, 0.30, lum) * 0.25;   // shadow → mid
    shade += smoothstep(0.30, 0.55, lum) * 0.25;   // mid → lit
    shade += smoothstep(0.55, 0.75, lum) * 0.20;   // lit → highlight

    // Apply shading tiers: multiply original hue by the flat shade value
    // This keeps colors intact but forces them into distinct bands
    float shade_scale = (shade + 0.15) / max(lum, 0.01);
    shade_scale = clamp(shade_scale, 0.0, 2.0);
    color = color * shade_scale;

    // --- Boost saturation for that animated pop ---
    float qlum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(qlum), color, 1.5);

    // Slight warm tint in shadows (ink wash feel)
    color += vec3(0.06, 0.03, 0.0) * (1.0 - lum) * 0.5;

    // --- Bold black ink outlines ---
    color = mix(color, vec3(0.0), edge * 0.92);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, c, detail * 0.65);

    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
