#version 330 core
// Terrain and water chunks: vertices are already in world space.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in float aMaterial;

uniform mat4 uViewProj;
uniform mat4 uLightViewProj;
uniform float uWave;   // water: height of the waves (0 for terrain)
uniform float uTime;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
flat out float vMaterial;
out vec4 vLightPos;

void main() {
    vec3 p = aPos;
    if (uWave > 0.0 && aNormal.y > 0.5) {
        p.y += (sin(p.x * 0.35 + uTime * 1.3) + sin(p.z * 0.29 - uTime * 1.1)) * 0.5 * uWave;
    }
    vWorldPos = p;
    vNormal = aNormal;
    vColor = aColor;
    vMaterial = aMaterial;
    vLightPos = uLightViewProj * vec4(p, 1.0);
    gl_Position = uViewProj * vec4(p, 1.0);
}
