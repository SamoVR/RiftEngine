#version 330 core
// Terrain: the blended material colours from the mesh, the material's
// procedural detail (materials.glsl) and the shared lighting.
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
flat in float vMaterial;
in vec4 vLightPos;
out vec4 FragColor;

#include "lighting.glsl"
#include "materials.glsl"

uniform float uTime;

void main() {
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 V = normalize(uCameraPos - vWorldPos);
    int material = int(vMaterial + 0.5);
    MaterialLook look = materialLook(material, toLinear(vColor), vWorldPos * 0.5, N, uTime);
    vec3 color = shadeSurface(vWorldPos, vLightPos, N, V, look.albedo, look.roughness, look.specular, look.metallic, look.reflect);
    color += look.albedo * look.emissive * 2.0;
    FragColor = vec4(finishColor(color, vWorldPos, vec3(0.0), 0.0), 1.0);
}
