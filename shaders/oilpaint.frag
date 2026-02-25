#version 330 core

// Oil painting effect:
// Kuwahara filter for painterly brush strokes + color quantization

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
    int radius = 4;

    // Kuwahara filter: divide neighborhood into 4 quadrants
    // Pick the quadrant with lowest variance (smoothest region)
    vec3 mean[4];
    float var[4];

    for (int q = 0; q < 4; q++) {
        mean[q] = vec3(0.0);
        var[q] = 0.0;
    }

    float count = 0.0;
    // Accumulate for each quadrant
    for (int i = 0; i <= radius; i++) {
        for (int j = 0; j <= radius; j++) {
            // Q0: top-left
            vec3 s0 = texture(u_screen, uv + vec2(-float(i), -float(j)) * px).rgb;
            mean[0] += s0;
            // Q1: top-right
            vec3 s1 = texture(u_screen, uv + vec2( float(i), -float(j)) * px).rgb;
            mean[1] += s1;
            // Q2: bottom-left
            vec3 s2 = texture(u_screen, uv + vec2(-float(i),  float(j)) * px).rgb;
            mean[2] += s2;
            // Q3: bottom-right
            vec3 s3 = texture(u_screen, uv + vec2( float(i),  float(j)) * px).rgb;
            mean[3] += s3;
            count += 1.0;
        }
    }

    for (int q = 0; q < 4; q++) {
        mean[q] /= count;
    }

    // Second pass: compute variance for each quadrant
    for (int i = 0; i <= radius; i++) {
        for (int j = 0; j <= radius; j++) {
            vec3 s0 = texture(u_screen, uv + vec2(-float(i), -float(j)) * px).rgb;
            vec3 d0 = s0 - mean[0]; var[0] += dot(d0, d0);

            vec3 s1 = texture(u_screen, uv + vec2( float(i), -float(j)) * px).rgb;
            vec3 d1 = s1 - mean[1]; var[1] += dot(d1, d1);

            vec3 s2 = texture(u_screen, uv + vec2(-float(i),  float(j)) * px).rgb;
            vec3 d2 = s2 - mean[2]; var[2] += dot(d2, d2);

            vec3 s3 = texture(u_screen, uv + vec2( float(i),  float(j)) * px).rgb;
            vec3 d3 = s3 - mean[3]; var[3] += dot(d3, d3);
        }
    }

    // Pick quadrant with minimum variance
    int best = 0;
    float best_var = var[0];
    for (int q = 1; q < 4; q++) {
        if (var[q] < best_var) {
            best_var = var[q];
            best = q;
        }
    }

    vec3 color = mean[best];

    // Slight color boost
    float lum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(lum), color, 1.25);

    // Text preservation: keep original in high-detail areas
    float detail = textDetect(uv, px);
    color = mix(color, orig, detail * 0.75);

    frag_color = vec4(color, 1.0);
}
