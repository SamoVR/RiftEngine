#version 330 core
// Anti-aliased minor (step) and major (10 x step) lines, the X / Z axes in
// red / blue, fading out with distance from the camera.
in vec3 vWorldPos;
out vec4 FragColor;
uniform vec3 uCameraPos;
uniform float uStep;
uniform float uFadeDistance;
uniform vec3 uLineColor;

float gridLine(vec2 p, float spacing) {
    vec2 coord = p / spacing;
    vec2 d = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
    return 1.0 - min(min(d.x, d.y), 1.0);
}

void main() {
    vec2 p = vWorldPos.xz;
    float minor = gridLine(p, uStep) * 0.3;
    float major = gridLine(p, uStep * 10.0) * 0.65;
    float a = max(minor, major);
    vec3 color = uLineColor;
    vec2 axisWidth = fwidth(p) * 1.5;
    if (abs(p.y) < axisWidth.y) { color = vec3(0.86, 0.27, 0.27); a = 0.9; } // X axis (z = 0)
    if (abs(p.x) < axisWidth.x) { color = vec3(0.27, 0.47, 0.92); a = 0.9; } // Z axis (x = 0)
    float dist = length(vWorldPos - uCameraPos);
    a *= 1.0 - smoothstep(uFadeDistance * 0.35, uFadeDistance, dist);
    if (a < 0.01) discard;
    FragColor = vec4(color, a);
}
