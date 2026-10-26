// Procedural looks of Roblox's materials, shared by parts and terrain.
// p: position in studs (the part's own space, or the world for terrain),
// n: surface normal in the same space, t: time in seconds.
// Needs hash() and noise() from lighting.glsl.

struct MaterialLook {
    vec3 albedo;      // linear
    float roughness;
    float specular;
    float metallic;
    float reflect;    // extra sky reflection
    float emissive;   // > 0: glows (Neon, lava cracks)
    float alpha;
};

// The face's own plane (whichever axis the normal points along most).
vec2 planarUV(vec3 p, vec3 n) {
    vec3 a = abs(n);
    if (a.y >= a.x && a.y >= a.z) return p.xz;
    if (a.x >= a.z) return p.zy;
    return p.xy;
}

// Distance to the nearest of a grid of jittered points, and how close the
// second nearest is (small = near an edge between cells).
vec2 voronoi(vec2 x) {
    vec2 cell = floor(x), f = fract(x);
    float d1 = 8.0, d2 = 8.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(float(i), float(j));
            vec2 o = vec2(hash(vec3(cell + g, 1.7)), hash(vec3(cell + g, 4.3)));
            vec2 r = g + o - f;
            float d = dot(r, r);
            if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
        }
    return vec2(sqrt(d1), sqrt(d2) - sqrt(d1));
}

float fbm(vec3 p) { return noise(p) * 0.55 + noise(p * 2.07) * 0.28 + noise(p * 4.13) * 0.17; }

MaterialLook materialLook(int m, vec3 albedo, vec3 p, vec3 n, float t) {
    MaterialLook L;
    L.albedo = albedo;
    L.roughness = 0.8;   // Plastic: matte, like Roblox
    L.specular = 0.06;
    L.metallic = 0.0;
    L.reflect = 0.0;
    L.emissive = 0.0;
    L.alpha = 1.0;
    vec2 uv = planarUV(p, n);

    if (m == 1) {                                   // SmoothPlastic
        L.roughness = 0.5; L.specular = 0.14;   // a soft sheen, no mirror
    } else if (m == 2) {                            // Metal: faint brushed streaks
        L.albedo *= 0.92 + 0.12 * noise(vec3(uv.x * 0.6, uv.y * 30.0, 0.0));
        L.roughness = 0.32; L.specular = 0.9; L.metallic = 0.85;
    } else if (m == 3) {                            // Neon
        L.emissive = 1.0;
    } else if (m == 4) {                            // Glass
        L.roughness = 0.05; L.specular = 1.0; L.reflect = 0.35; L.alpha = 0.35;
    } else if (m == 5) {                            // Wood: rings along X
        float rings = fract(length(p.yz * vec2(1.0, 0.35)) * 2.2 + noise(p * vec3(0.4, 3.0, 3.0)) * 1.4);
        L.albedo *= mix(0.72, 1.08, smoothstep(0.0, 0.6, rings)) * mix(0.92, 1.05, noise(p * vec3(0.2, 9.0, 9.0)));
        L.roughness = 0.6; L.specular = 0.12;
    } else if (m == 6) {                            // Concrete: speckles
        L.albedo *= 0.86 + 0.18 * noise(p * 6.0) + 0.06 * noise(p * 23.0);
        L.roughness = 0.9; L.specular = 0.05;
    } else if (m == 7) {                            // Brick
        vec2 b = uv / vec2(1.0, 0.45);
        float row = floor(b.y);
        b.x += mod(row, 2.0) * 0.5;
        vec2 f = fract(b);
        float mortar = max(step(f.x, 0.05), step(f.y, 0.1));
        L.albedo *= mix(0.82 + 0.3 * hash(vec3(floor(b), 2.0)), 1.6, mortar) * (0.92 + 0.12 * noise(p * 8.0));
        if (mortar > 0.5) L.albedo = mix(L.albedo, vec3(0.55), 0.6);
        L.roughness = 0.85; L.specular = 0.06;
    } else if (m == 8) {                            // Cobblestone
        vec2 v = voronoi(uv * 1.3);
        float gap = smoothstep(0.02, 0.12, v.y);
        L.albedo *= mix(0.45, 0.85 + 0.3 * hash(vec3(floor(uv * 1.3 + 0.5), 3.0)), gap) * (0.9 + 0.15 * noise(p * 5.0));
        L.roughness = 0.8; L.specular = 0.08;
    } else if (m == 9) {                            // Granite: coloured speckles
        float s = noise(p * 11.0);
        L.albedo *= 0.8 + 0.35 * s;
        L.albedo = mix(L.albedo, vec3(0.9, 0.85, 0.82) * L.albedo * 1.4, step(0.78, noise(p * 19.0)));
        L.roughness = 0.45; L.specular = 0.3;
    } else if (m == 10) {                           // Marble: veins
        float vein = abs(sin(p.x * 1.2 + p.y * 0.6 + fbm(p * 0.9) * 6.0));
        L.albedo *= mix(0.72, 1.08, smoothstep(0.0, 0.25, vein));
        L.roughness = 0.18; L.specular = 0.5; L.reflect = 0.08;
    } else if (m == 11) {                           // Slate: layers
        L.albedo *= 0.8 + 0.25 * noise(vec3(uv.x * 0.8, uv.y * 7.0, 0.5)) + 0.1 * noise(p * 4.0);
        L.roughness = 0.7; L.specular = 0.12;
    } else if (m == 12) {                           // Fabric: weave
        float weave = sin(uv.x * 45.0) * sin(uv.y * 45.0);
        L.albedo *= 0.88 + 0.1 * weave + 0.06 * noise(p * 3.0);
        L.roughness = 0.95; L.specular = 0.03;
    } else if (m == 13) {                           // Foil: crinkled shiny metal
        L.albedo *= 0.75 + 0.45 * noise(p * 4.5);
        L.roughness = 0.12; L.specular = 0.95; L.metallic = 0.8; L.reflect = 0.15;
    } else if (m == 14) {                           // DiamondPlate: raised diamonds on metal
        vec2 d = uv * 2.5;
        vec2 cell = fract(vec2(d.x + d.y, d.x - d.y) * 0.5);
        float bump = smoothstep(0.35, 0.45, 1.0 - abs(cell.x - 0.5) * 2.0) * smoothstep(0.6, 0.9, 1.0 - abs(cell.y - 0.5) * 2.0);
        L.albedo *= 0.8 + 0.35 * bump;
        L.roughness = 0.35; L.specular = 0.85; L.metallic = 0.85;
    } else if (m == 15) {                           // CorrodedMetal: rust patches
        float rust = smoothstep(0.45, 0.65, fbm(p * 1.4));
        L.albedo = mix(L.albedo, vec3(0.32, 0.12, 0.04) * (0.8 + 0.4 * noise(p * 9.0)), rust);
        L.roughness = mix(0.35, 0.9, rust); L.specular = mix(0.8, 0.1, rust); L.metallic = mix(0.8, 0.1, rust);
    } else if (m == 16) {                           // WoodPlanks
        float plank = floor(uv.y / 0.7);
        float seam = step(fract(uv.y / 0.7), 0.05) + step(fract(uv.x / 4.0 + hash(vec3(plank, 0.0, 5.0))), 0.012);
        float grain = noise(vec3(uv.x * 0.7, uv.y * 14.0, plank));
        L.albedo *= (0.78 + 0.3 * hash(vec3(plank, 1.0, 7.0))) * (0.85 + 0.2 * grain) * mix(1.0, 0.45, min(seam, 1.0));
        L.roughness = 0.65; L.specular = 0.1;
    } else if (m == 17) {                           // Pebble
        vec2 v = voronoi(uv * 3.2);
        L.albedo *= mix(0.5, 0.8 + 0.35 * hash(vec3(floor(uv * 3.2 + 0.5), 8.0)), smoothstep(0.02, 0.1, v.y));
        L.roughness = 0.6; L.specular = 0.15;
    } else if (m == 18) {                           // Ice
        L.albedo *= 0.92 + 0.12 * noise(p * 2.0);
        L.roughness = 0.08; L.specular = 0.7; L.reflect = 0.2; L.alpha = 0.88;
    } else if (m == 19) {                           // ForceField: shimmering
        float wave = 0.5 + 0.5 * sin(dot(p, vec3(2.3, 3.1, 1.7)) + t * 3.0);
        L.emissive = 0.6 + 0.6 * wave;
        L.alpha = 0.25 + 0.35 * wave * noise(p * 3.0 + t);
    } else if (m == 20 || m == 21) {                // Grass, LeafyGrass
        float blades = noise(vec3(uv * vec2(9.0, 9.0), 0.0)) * 0.6 + noise(p * 1.1) * 0.4;
        L.albedo *= 0.78 + 0.38 * blades;
        if (m == 21) L.albedo *= 0.9 + 0.2 * step(0.6, noise(p * 3.0));
        L.roughness = 0.9; L.specular = 0.05;
    } else if (m == 22) {                           // Sand: fine grain and ripples
        float ripple = 0.5 + 0.5 * sin(p.x * 2.4 + noise(p * 0.7) * 4.0);
        L.albedo *= 0.88 + 0.1 * noise(p * 22.0) + 0.06 * ripple;
        L.roughness = 0.95; L.specular = 0.04;
    } else if (m == 23) {                           // Sandstone: strata
        L.albedo *= 0.82 + 0.2 * noise(vec3(p.x * 0.3, p.y * 4.0, p.z * 0.3)) + 0.06 * noise(p * 9.0);
        L.roughness = 0.85; L.specular = 0.06;
    } else if (m == 24) {                           // Rock
        float r = fbm(p * 0.8);
        L.albedo *= 0.7 + 0.45 * r + 0.08 * noise(p * 9.0);
        L.roughness = 0.8; L.specular = 0.1;
    } else if (m == 25) {                           // Ground: dirt
        L.albedo *= 0.8 + 0.3 * fbm(p * 1.6);
        L.roughness = 0.95; L.specular = 0.04;
    } else if (m == 26) {                           // Mud: dark and wet
        L.albedo *= 0.85 + 0.25 * fbm(p * 1.2);
        L.roughness = 0.35; L.specular = 0.35;
    } else if (m == 27) {                           // Snow: soft with sparkles
        L.albedo *= 0.94 + 0.08 * noise(p * 2.0);
        L.roughness = 0.75; L.specular = 0.15 + 0.6 * step(0.985, hash(floor(p * 20.0)));
    } else if (m == 28) {                           // Glacier
        L.albedo *= 0.85 + 0.25 * fbm(p * 0.6);
        L.roughness = 0.2; L.specular = 0.6; L.reflect = 0.12;
    } else if (m == 29) {                           // Basalt: columns
        vec2 v = voronoi(uv * 0.9);
        L.albedo *= mix(0.6, 1.1, smoothstep(0.02, 0.09, v.y)) * (0.9 + 0.15 * noise(p * 6.0));
        L.roughness = 0.7; L.specular = 0.15;
    } else if (m == 30) {                           // Asphalt
        L.albedo *= 0.82 + 0.25 * noise(p * 14.0) + 0.1 * noise(p * 1.5);
        L.roughness = 0.9; L.specular = 0.06;
    } else if (m == 31) {                           // Limestone
        L.albedo *= 0.88 + 0.16 * fbm(p * 1.1);
        L.roughness = 0.8; L.specular = 0.08;
    } else if (m == 32) {                           // Pavement: square slabs
        vec2 f = fract(uv / 2.0);
        float seam = max(step(f.x, 0.03), step(f.y, 0.03));
        L.albedo *= (0.9 + 0.15 * hash(vec3(floor(uv / 2.0), 9.0))) * mix(1.0, 0.6, seam) * (0.94 + 0.08 * noise(p * 10.0));
        L.roughness = 0.85; L.specular = 0.06;
    } else if (m == 33) {                           // Salt: crystals
        L.albedo *= 0.9 + 0.15 * step(0.7, noise(p * 16.0)) + 0.05 * noise(p * 3.0);
        L.roughness = 0.5; L.specular = 0.3;
    } else if (m == 34) {                           // CrackedLava: glowing cracks in dark rock
        vec2 v = voronoi(uv * 0.8 + vec2(0.0, t * 0.02));
        float crack = 1.0 - smoothstep(0.0, 0.06, v.y);
        L.albedo = mix(vec3(0.05, 0.04, 0.04) * (0.8 + 0.4 * noise(p * 4.0)), L.albedo * 1.2, crack);
        L.emissive = crack * (0.8 + 0.3 * sin(t * 2.0 + p.x));
        L.roughness = 0.75; L.specular = 0.1;
    } else if (m == 35) {                           // Water (on a part)
        L.roughness = 0.05; L.specular = 0.8; L.reflect = 0.3; L.alpha = 0.6;
    }
    return L;
}
