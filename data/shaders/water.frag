#version 330 core
// Terrain water: Terrain.WaterColor / WaterTransparency, moving ripples,
// sky reflections that grow at grazing angles, the sun's glint.
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
flat in float vMaterial;
in vec4 vLightPos;
out vec4 FragColor;

#include "lighting.glsl"

uniform vec3 uWaterColor;       // sRGB
uniform float uWaterTransparency;
uniform float uTime;

void main() {
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    // Ripples: two scrolling noise layers tilt the normal.
    vec3 p = vWorldPos * 0.6;
    float nx = noise(p + vec3(uTime * 0.5, 0.0, uTime * 0.3)) - noise(p * 1.7 - vec3(uTime * 0.4, 0.0, -uTime * 0.2));
    float nz = noise(p.zyx + vec3(uTime * 0.35, 3.0, uTime * 0.45)) - noise(p.zyx * 1.9 + vec3(-uTime * 0.3, 7.0, uTime * 0.25));
    N = normalize(N + vec3(nx, 0.0, nz) * 0.18);
    vec3 V = normalize(uCameraPos - vWorldPos);

    vec3 albedo = toLinear(uWaterColor);
    vec3 color = shadeSurface(vWorldPos, vLightPos, N, V, albedo, 0.06, 0.8, 0.0, 0.0);
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    color = mix(color, skyAt(reflect(-V, N)), 0.15 + 0.6 * fresnel);
    float alpha = clamp((1.0 - uWaterTransparency) + fresnel * 0.6, 0.15, 0.97);
    FragColor = vec4(finishColor(color, vWorldPos, vec3(0.0), 0.0), alpha);
}
