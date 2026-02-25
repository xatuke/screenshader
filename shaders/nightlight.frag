#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_screen;
uniform vec2 u_resolution;
uniform float u_time;

void main() {
    vec3 color = texture(u_screen, v_texcoord).rgb;

    // Reduce blue channel, warm shift
    color.b *= 0.5;
    color.r *= 1.05;
    color.g *= 0.97;

    frag_color = vec4(color, 1.0);
}
