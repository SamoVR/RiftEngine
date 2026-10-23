#version 330 core
// Editor ground grid: a big quad on y = 0 that follows the camera.
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
uniform vec3 uCameraPos;
uniform float uExtent;
out vec3 vWorldPos;
void main() {
    vec3 p = vec3(aPos.x * uExtent + uCameraPos.x, 0.0, aPos.z * uExtent + uCameraPos.z);
    vWorldPos = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
