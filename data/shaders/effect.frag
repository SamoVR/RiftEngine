#version 330 core
// Particles, beams and trails. Output is premultiplied: LightEmission 0
// blends like normal transparency, 1 adds its light to what is behind.
in vec2 vUV;
in vec4 vColor;
in float vEmission;
flat in float vMode;   // 0 textured, 1 soft dot, 2 solid band, 3 premultiplied texture (SurfaceGui)
out vec4 FragColor;

uniform sampler2D uTexture;
uniform int uTextured;  // 0: the texture failed to load (fall back to the soft dot)

void main() {
    if (vMode > 2.5) {
        // Already premultiplied (GUI drawn into a texture); the colour tints it.
        vec4 t = texture(uTexture, vUV);
        if (t.a < 0.003) discard;
        FragColor = vec4(t.rgb * vColor.rgb, t.a) * vColor.a;
        return;
    }
    float mode = vMode;
    if (mode < 0.5 && uTextured == 0) mode = 1.0;
    vec4 tex = vec4(1.0);
    if (mode < 0.5) {
        tex = texture(uTexture, vUV);
    } else if (mode < 1.5) {
        // A round dot with a soft edge and a brighter middle.
        float d = clamp(length(vUV - vec2(0.5)) * 2.0, 0.0, 1.0);
        float a = 1.0 - d;
        tex = vec4(1.0, 1.0, 1.0, a * a * (3.0 - 2.0 * a));
    }
    vec4 c = vColor * tex;
    if (c.a < 0.003) discard;
    FragColor = vec4(c.rgb * c.a, c.a * (1.0 - clamp(vEmission, 0.0, 1.0)));
}
