#version 330 core
// Sky gradient, sun disk and glow, darker ground below the horizon, stars at night.
in vec2 vNdc;
out vec4 FragColor;
uniform mat4 uInvViewProj;
uniform vec3 uSunDirection;
uniform vec3 uSunColor;     // linear, * intensity
uniform vec3 uSkyColor;     // linear
uniform vec3 uHorizonColor; // linear
uniform vec3 uGroundColor;  // linear
uniform float uExposure;
uniform float uDay;         // 0 night .. 1 day

void main() {
    vec4 nearP = uInvViewProj * vec4(vNdc, -1.0, 1.0);
    vec4 farP = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    float up = dir.y;
    vec3 color;
    if (up >= 0.0) color = mix(uHorizonColor, uSkyColor, pow(up, 0.35));
    else color = mix(uHorizonColor, uGroundColor, min(-up * 4.0, 1.0));
    float s = max(dot(dir, uSunDirection), 0.0);
    color += uSunColor * (pow(s, 1200.0) * 40.0 + pow(s, 12.0) * 0.25) * step(-0.02, uSunDirection.y);
    if (uDay < 0.5 && up > 0.0) {
        vec3 cell = floor(dir * 180.0);
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        color += vec3(step(0.997, h)) * (0.5 - uDay) * 2.0;
    }
    color *= uExposure;
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    FragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
