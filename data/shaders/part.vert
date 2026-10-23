#version 330 core
// Parts: world-space lighting data for part.frag.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec3 aNormal;

uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform mat4 uViewProj;
uniform mat4 uLightViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec3 vLocalPos;
out vec3 vLocalNormal;
out vec4 vLightPos;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(uNormalMatrix * aNormal);
    vUV = aUV;
    vLocalPos = aPos;
    vLocalNormal = aNormal;
    vLightPos = uLightViewProj * world;
    gl_Position = uViewProj * world;
}
