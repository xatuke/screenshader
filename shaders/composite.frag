#version 330 core

in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_texture;

void main() {
    vec2 tc = vec2(v_texcoord.x, 1.0 - v_texcoord.y);
    frag_color = texture(u_texture, tc);
}
