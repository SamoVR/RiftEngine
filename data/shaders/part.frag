#version 330 core
// Parts: materials on top of the shared lighting (lighting.glsl).
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec3 vLocalPos;
in vec3 vLocalNormal;
in vec4 vLightPos;
out vec4 FragColor;

#include "lighting.glsl"

// Material
uniform vec3 uColor;
uniform float uTransparency;
uniform int uMaterial;          // Enum.Material order (Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete, ...)
uniform float uReflectance;
uniform sampler2D uTexture;
uniform int uHasTexture;
uniform vec2 uTextureScale;     // studs per repeat; 0 = stretch over each face
uniform vec3 uSize;             // part size in studs
uniform vec3 uTint;             // editor highlight
uniform float uTintAmount;
uniform float uTime;            // seconds (ForceField shimmer)

// Texture coordinates in studs: each face is projected on its own plane, so
// textures tile the same on a 1-stud cube and on a 64-stud baseplate.
vec2 studUV() {
    vec3 p = vLocalPos * uSize;
    vec3 n = abs(vLocalNormal);
    vec2 uv;
    if (n.y >= n.x && n.y >= n.z) uv = p.xz;
    else if (n.x >= n.z) uv = p.zy;
    else uv = p.xy;
    return uv / max(uTextureScale, vec2(0.001));
}

#include "materials.glsl"

void main() {
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 V = normalize(uCameraPos - vWorldPos);

    vec3 albedo = toLinear(uColor);
    if (uHasTexture == 1) {
        vec2 uv = (uTextureScale.x > 0.0) ? studUV() : vUV;
        albedo *= toLinear(texture(uTexture, uv).rgb);
    }

    MaterialLook look = materialLook(uMaterial, albedo, vLocalPos * uSize, vLocalNormal, uTime);
    float alpha = (1.0 - uTransparency) * look.alpha;

    vec3 color;
    if (uMaterial == 3) { // Neon: all glow
        color = look.albedo * 2.2 + look.albedo * 0.3 * pow(1.0 - max(dot(N, V), 0.0), 2.0);
    } else {
        color = shadeSurface(vWorldPos, vLightPos, N, V, look.albedo, look.roughness, look.specular, look.metallic,
                             uReflectance + look.reflect);
        color += look.albedo * look.emissive * 2.0; // lava cracks, force fields
    }
    if (uMaterial == 4) { // Glass: more opaque at grazing angles
        float fresnel = pow(1.0 - max(dot(N, V), 0.0), 5.0);
        alpha = clamp(alpha + fresnel * 0.5, 0.0, 1.0);
    }
    FragColor = vec4(finishColor(color, vWorldPos, uTint, uTintAmount), alpha);
}
