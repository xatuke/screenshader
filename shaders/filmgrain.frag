#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec2 uv = v_texcoord;
    vec3 color = texture(u_screen, uv).rgb;

    // Slight desaturation (vintage look)
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, 0.7);

    // Warm vintage color grade
    color.r *= 1.08;
    color.g *= 1.02;
    color.b *= 0.88;

    // Lift shadows, compress highlights (low contrast vintage feel)
    color = color * 0.85 + 0.06;

    // Film grain
    float grain = hash(uv * u_resolution + fract(u_time * 13.37)) * 2.0 - 1.0;
    color += grain * 0.08;

    // Film scratches (vertical lines that appear briefly)
    float scratch_x = hash(vec2(floor(u_time * 24.0), 0.0));
    float scratch_width = 0.001;
    float scratch = smoothstep(scratch_width, 0.0, abs(uv.x - scratch_x)) * 0.15;
    float scratch_visible = step(0.92, hash(vec2(floor(u_time * 24.0), 1.0)));
    color += scratch * scratch_visible;

    // Frame flicker
    float flicker = 1.0 + (hash(vec2(floor(u_time * 24.0), 2.0)) - 0.5) * 0.03;
    color *= flicker;

    // Vignette (strong, like old film)
    vec2 vig = uv * (1.0 - uv);
    float vignette = vig.x * vig.y * 15.0;
    vignette = clamp(pow(vignette, 0.35), 0.0, 1.0);
    color *= mix(0.5, 1.0, vignette);

    // Slight sepia tint in dark areas
    float sepia_amount = (1.0 - lum) * 0.15;
    vec3 sepia = vec3(1.2, 1.0, 0.8) * lum;
    color = mix(color, sepia, sepia_amount);

    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
