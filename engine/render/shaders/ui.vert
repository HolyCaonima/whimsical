#version 460
layout(location=0) in vec2 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 texcoord;
layout(location=0) out vec4 vertexColor;
layout(location=1) out vec2 uv;
layout(push_constant) uniform Push { mat4 transform; vec2 translation; vec2 viewport; } p;
void main() {
    vec4 point=p.transform*vec4(position+p.translation,0,1);
    gl_Position=vec4(point.xy*2.0/p.viewport-point.ww,0,point.w);
    vertexColor=color; uv=texcoord;
}
