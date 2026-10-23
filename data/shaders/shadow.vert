#version 330 core
// Sun shadow map: depth only.
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uLightViewProj;
void main() { gl_Position = uLightViewProj * uModel * vec4(aPos, 1.0); }
