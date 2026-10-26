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
uniform vec3 uMoonDirection;

float moonHash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453); }
float moonNoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(moonHash(i), moonHash(i + vec3(1, 0, 0)), f.x), mix(moonHash(i + vec3(0, 1, 0)), moonHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(moonHash(i + vec3(0, 0, 1)), moonHash(i + vec3(1, 0, 1)), f.x), mix(moonHash(i + vec3(0, 1, 1)), moonHash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

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
    // The moon: a pale disc with darker seas and craters, and a faint halo.
    float night = clamp(1.0 - uDay * 1.6, 0.0, 1.0);
    if (night > 0.0) {
        float m = dot(dir, uMoonDirection);
        const float radius = 0.0175; // about 1 degree
        vec3 right = normalize(cross(uMoonDirection, abs(uMoonDirection.y) > 0.99 ? vec3(1, 0, 0) : vec3(0, 1, 0)));
        vec3 upv = cross(right, uMoonDirection);
        vec2 q = vec2(dot(dir, right), dot(dir, upv)) / radius; // -1..1 across the disc
        float r = length(q);
        float disc = (1.0 - smoothstep(0.92, 1.0, r)) * step(0.0, m);
        if (disc > 0.0) {
            float seas = smoothstep(0.45, 0.7, moonNoise(vec3(q * 2.3, 1.7)));
            float craters = smoothstep(0.62, 0.8, moonNoise(vec3(q * 7.0, 4.1))) * 0.35;
            float limb = sqrt(max(1.0 - r * r, 0.0));             // a little darker at the edge
            vec3 moon = vec3(0.92, 0.93, 0.98) * (1.0 - seas * 0.35 - craters) * mix(0.75, 1.0, limb);
            color = mix(color, moon * 1.6, disc * night);
        }
        color += vec3(0.55, 0.62, 0.85) * pow(max(m, 0.0), 900.0) * 0.6 * night;
    }
    color *= uExposure;
    color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    FragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
