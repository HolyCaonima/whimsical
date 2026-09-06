#version 460
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) in vec4 vertexColor;
layout(location=1) in vec2 uv;
layout(location=0) out vec4 color;
void main() { color=vertexColor*texture(image,uv); }
