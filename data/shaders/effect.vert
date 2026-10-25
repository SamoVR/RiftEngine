#version 330 core
// Particles, beams and trails: world-space quads built on the CPU.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aEmission;
layout(location = 4) in float aMode;

uniform mat4 uViewProj;

out vec2 vUV;
out vec4 vColor;
out float vEmission;
flat out float vMode;

void main() {
    vUV = aUV;
    vColor = aColor;
    vEmission = aEmission;
    vMode = aMode;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
