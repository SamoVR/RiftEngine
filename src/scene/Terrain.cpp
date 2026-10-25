#include "scene/Terrain.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

namespace rift {

namespace {
    std::atomic<uint64_t> g_uid{ 1 };
    std::atomic<uint64_t> g_revision{ 1 }; // one counter for every terrain: restored chunks always look new
    constexpr int N = TerrainData::kChunkSize;
    constexpr float S = TerrainData::kVoxelSize;
    constexpr float kIso = 0.5f;
    constexpr uint64_t kMaxEditVoxels = 16u * 1024u * 1024u;

    int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
    int local(int x, int y, int z) { return (z * N + y) * N + x; }
    bool solidMaterial(uint8_t m) { return m != static_cast<uint8_t>(Material::Air) && m != static_cast<uint8_t>(Material::Water); }

    // ---- noise (gradient noise, hashed lattice)
    uint32_t hash3(int x, int y, int z, uint32_t seed) {
        uint32_t h = seed * 0x9E3779B9u ^ static_cast<uint32_t>(x) * 0x85EBCA6Bu ^ static_cast<uint32_t>(y) * 0xC2B2AE35u ^ static_cast<uint32_t>(z) * 0x27D4EB2Fu;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
        return h;
    }
    float grad(uint32_t h, float x, float y, float z) {
        switch (h & 15u) {
            case 0: return x + y; case 1: return -x + y; case 2: return x - y; case 3: return -x - y;
            case 4: return x + z; case 5: return -x + z; case 6: return x - z; case 7: return -x - z;
            case 8: return y + z; case 9: return -y + z; case 10: return y - z; case 11: return -y - z;
            case 12: return x + y; case 13: return -y + z; case 14: return -x + y; default: return -y - z;
        }
    }
    float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    float noise3(float x, float y, float z, uint32_t seed) {
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y)), iz = static_cast<int>(std::floor(z));
        const float fx = x - ix, fy = y - iy, fz = z - iz;
        const float u = fade(fx), v = fade(fy), w = fade(fz);
        auto g = [&](int dx, int dy, int dz) { return grad(hash3(ix + dx, iy + dy, iz + dz, seed), fx - dx, fy - dy, fz - dz); };
        const float x00 = g(0, 0, 0) + u * (g(1, 0, 0) - g(0, 0, 0));
        const float x10 = g(0, 1, 0) + u * (g(1, 1, 0) - g(0, 1, 0));
        const float x01 = g(0, 0, 1) + u * (g(1, 0, 1) - g(0, 0, 1));
        const float x11 = g(0, 1, 1) + u * (g(1, 1, 1) - g(0, 1, 1));
        const float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
        return (y0 + w * (y1 - y0)) * 0.7f; // about -1..1
    }
    float fbm2(float x, float z, int octaves, uint32_t seed) {
        float sum = 0.0f, amp = 1.0f, norm = 0.0f;
        for (int o = 0; o < octaves; ++o) {
            sum += noise3(x, 0.37f * static_cast<float>(o), z, seed + static_cast<uint32_t>(o) * 101u) * amp;
            norm += amp;
            amp *= 0.5f;
            x *= 2.03f;
            z *= 2.03f;
        }
        return sum / norm;
    }

    // How much of a voxel (by its centre) is inside a shape whose signed
    // distance is `d` (negative inside): a one-voxel-wide smooth edge.
    float coverage(float d) { return std::clamp(0.5f - d / S, 0.0f, 1.0f); }
    uint8_t toOcc(float f) { return static_cast<uint8_t>(std::clamp(std::lround(f * 255.0f), 0L, 255L)); }

    // Filling with a material (Air = carving).
    void fillVoxel(Voxel& v, float cov, Material material) {
        if (cov <= 0.0f) return;
        if (material == Material::Air) {
            const uint8_t occ = std::min(v.occupancy, toOcc(1.0f - cov));
            v.occupancy = occ;
            if (occ == 0) v.material = static_cast<uint8_t>(Material::Air);
            return;
        }
        const uint8_t occ = toOcc(cov);
        if (occ >= v.occupancy || v.material == static_cast<uint8_t>(Material::Air)) v.material = static_cast<uint8_t>(material);
        v.occupancy = std::max(v.occupancy, occ);
    }
}

bool TerrainData::Chunk::allAir() const {
    for (const Voxel& v : voxels) if (v.occupancy) return false;
    return true;
}

TerrainData::TerrainData() : uid_(g_uid++) {
    for (int m = 0; m < kMaterialCount; ++m) colors_[static_cast<size_t>(m)] = defaultMaterialColor(static_cast<Material>(m));
}

TerrainData::TerrainData(const TerrainData& other)
    : uid_(g_uid++), revision_(other.revision_), chunks_(other.chunks_), colors_(other.colors_), customColor_(other.customColor_) {}

TerrainData& TerrainData::operator=(const TerrainData& other) {
    if (this == &other) return *this;
    // The uid stays: it names this object, not its contents. New revisions,
    // so caches of this terrain rebuild.
    chunks_ = other.chunks_;
    colors_ = other.colors_;
    customColor_ = other.customColor_;
    for (auto& [key, c] : chunks_) c.revision = g_revision++;
    revision_ = g_revision++;
    recording_ = false;
    recorded_.clear();
    lastTouched_.reset();
    touched_.clear();
    return *this;
}

bool TerrainData::empty() const {
    for (const auto& [key, c] : chunks_) if (!c.allAir()) return false;
    return true;
}

// ============================================================================
//  Voxels
// ============================================================================
ChunkKey TerrainData::chunkOf(int x, int y, int z) { return { floorDiv(x, N), floorDiv(y, N), floorDiv(z, N) }; }

glm::ivec3 TerrainData::voxelAt(const glm::vec3& world) {
    return { static_cast<int>(std::floor(world.x / S)), static_cast<int>(std::floor(world.y / S)), static_cast<int>(std::floor(world.z / S)) };
}

glm::vec3 TerrainData::voxelCenter(const glm::ivec3& v) { return (glm::vec3(v) + 0.5f) * S; }

TerrainData::Chunk* TerrainData::chunk(const ChunkKey& key, bool create) {
    auto it = chunks_.find(key);
    if (it != chunks_.end()) return &it->second;
    if (!create) return nullptr;
    if (recording_ && !recorded_.count(key)) recorded_[key] = std::nullopt;
    return &chunks_[key];
}

const TerrainData::Chunk* TerrainData::chunk(const ChunkKey& key) const {
    auto it = chunks_.find(key);
    return it == chunks_.end() ? nullptr : &it->second;
}

Voxel TerrainData::voxel(int x, int y, int z) const {
    const ChunkKey key = chunkOf(x, y, z);
    const Chunk* c = chunk(key);
    if (!c) return {};
    return c->voxels[static_cast<size_t>(local(x - key.x * N, y - key.y * N, z - key.z * N))];
}

void TerrainData::setVoxel(int x, int y, int z, const Voxel& v) {
    const ChunkKey key = chunkOf(x, y, z);
    Chunk* c = chunk(key, v.occupancy > 0);
    if (!c) return; // air where nothing is: no change
    Voxel& slot = c->voxels[static_cast<size_t>(local(x - key.x * N, y - key.y * N, z - key.z * N))];
    if (slot == v) return;
    if (recording_ && !recorded_.count(key)) recorded_[key] = *c;
    slot = v;
    if (slot.occupancy == 0) slot.material = static_cast<uint8_t>(Material::Air);
    const int lx = x - key.x * N, ly = y - key.y * N, lz = z - key.z * N;
    const bool edge = lx == 0 || ly == 0 || lz == 0 || lx == N - 1 || ly == N - 1 || lz == N - 1;
    if (!edge) {
        if (lastTouched_ && *lastTouched_ == key) return;
        touched_.insert(key);
        lastTouched_ = key;
        return;
    }
    for (int dz = lz == 0 ? -1 : 0; dz <= (lz == N - 1 ? 1 : 0); ++dz)
        for (int dy = ly == 0 ? -1 : 0; dy <= (ly == N - 1 ? 1 : 0); ++dy)
            for (int dx = lx == 0 ? -1 : 0; dx <= (lx == N - 1 ? 1 : 0); ++dx) touched_.insert({ key.x + dx, key.y + dy, key.z + dz });
}

void TerrainData::touchAround(const ChunkKey& k) {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) touched_.insert({ k.x + dx, k.y + dy, k.z + dz });
}

void TerrainData::finishEdit() {
    if (touched_.empty()) return;
    std::vector<ChunkKey> bump(touched_.begin(), touched_.end());
    touched_.clear();
    lastTouched_.reset();
    const size_t changed = bump.size();
    for (size_t n = 0; n < changed; ++n) {
        // The surface between this chunk and the ones below it (-x, -y, -z)
        // belongs to those: they must exist to be meshed.
        const ChunkKey k = bump[n];
        auto it = chunks_.find(k);
        if (it == chunks_.end() || it->second.allAir()) continue;
        for (int m = 1; m < 8; ++m) {
            const ChunkKey below{ k.x - (m & 1), k.y - ((m >> 1) & 1), k.z - ((m >> 2) & 1) };
            if (chunks_.count(below)) continue;
            chunk(below, true);
            bump.push_back(below);
        }
    }
    for (const ChunkKey& k : bump) {
        auto it = chunks_.find(k);
        if (it != chunks_.end()) it->second.revision = g_revision++;
    }
    revision_ = g_revision++;
}

void TerrainData::editVoxels(const glm::vec3& lo, const glm::vec3& hi, const std::function<void(const glm::ivec3&, const glm::vec3&, Voxel&)>& f) {
    const glm::ivec3 a = voxelAt(lo), b = voxelAt(hi);
    const uint64_t count = static_cast<uint64_t>(std::max(0, b.x - a.x + 1)) * static_cast<uint64_t>(std::max(0, b.y - a.y + 1))
                         * static_cast<uint64_t>(std::max(0, b.z - a.z + 1));
    if (count == 0 || count > kMaxEditVoxels) return; // nothing, or unreasonably large
    for (int z = a.z; z <= b.z; ++z)
        for (int y = a.y; y <= b.y; ++y)
            for (int x = a.x; x <= b.x; ++x) {
                Voxel v = voxel(x, y, z);
                const Voxel before = v;
                f({ x, y, z }, voxelCenter({ x, y, z }), v);
                if (!(v == before)) setVoxel(x, y, z, v);
            }
    finishEdit();
}

// ============================================================================
//  Roblox API
// ============================================================================
void TerrainData::fillBall(const glm::vec3& center, float radius, Material material) {
    const glm::vec3 r(radius + S);
    editVoxels(center - r, center + r, [&](const glm::ivec3&, const glm::vec3& p, Voxel& v) {
        fillVoxel(v, coverage(glm::length(p - center) - radius), material);
    });
}

void TerrainData::fillBlock(const glm::mat4& frame, const glm::vec3& size, const Material material) {
    const glm::vec3 half = size * 0.5f;
    const glm::mat4 inv = glm::inverse(frame);
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (int c = 0; c < 8; ++c) {
        const glm::vec3 corner((c & 1) ? half.x : -half.x, (c & 2) ? half.y : -half.y, (c & 4) ? half.z : -half.z);
        const glm::vec3 w = glm::vec3(frame * glm::vec4(corner, 1.0f));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
    editVoxels(lo - S, hi + S, [&](const glm::ivec3&, const glm::vec3& p, Voxel& v) {
        const glm::vec3 q = glm::abs(glm::vec3(inv * glm::vec4(p, 1.0f))) - half;
        fillVoxel(v, coverage(std::max({ q.x, q.y, q.z })), material);
    });
}

void TerrainData::fillCylinder(const glm::mat4& frame, float height, float radius, Material material) {
    const glm::mat4 inv = glm::inverse(frame);
    const float reach = std::max(radius, height * 0.5f) + S;
    const glm::vec3 center(frame[3]);
    editVoxels(center - glm::vec3(reach), center + glm::vec3(reach), [&](const glm::ivec3&, const glm::vec3& p, Voxel& v) {
        const glm::vec3 q = glm::vec3(inv * glm::vec4(p, 1.0f));
        const float d = std::max(glm::length(glm::vec2(q.x, q.z)) - radius, std::abs(q.y) - height * 0.5f);
        fillVoxel(v, coverage(d), material);
    });
}

void TerrainData::clear() {
    for (auto& [key, c] : chunks_) {
        if (recording_ && !recorded_.count(key)) recorded_[key] = c;
    }
    chunks_.clear();
    revision_ = g_revision++;
}

// ============================================================================
//  Brushes
// ============================================================================
void TerrainData::applyDab(const Dab& d) {
    const float strength = std::clamp(d.strength, 0.0f, 1.0f);
    const glm::vec3 reach(d.radius + S);
    // Shape: inside 1, with a smooth one-voxel edge (and a soft falloff for sculpting).
    auto shape = [&](const glm::vec3& p) {
        const glm::vec3 q = p - d.center;
        const float dist = d.cube ? std::max({ std::abs(q.x), std::abs(q.y), std::abs(q.z) }) : glm::length(q);
        return coverage(dist - d.radius);
    };
    auto soft = [&](const glm::vec3& p) {
        const glm::vec3 q = p - d.center;
        const float dist = d.cube ? std::max({ std::abs(q.x), std::abs(q.y), std::abs(q.z) }) : glm::length(q);
        const float t = std::clamp(dist / std::max(d.radius, 0.01f), 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    };
    const bool sculpt = d.brush == Brush::Grow || d.brush == Brush::Erode || d.brush == Brush::Smooth;
    // Smoothing reads the neighbours as they were before this dab.
    TerrainData* self = this;
    auto occAt = [self](int x, int y, int z) {
        const Voxel v = self->voxel(x, y, z);
        return solidMaterial(v.material) ? v.occupancy / 255.0f : 0.0f;
    };
    std::unordered_map<int64_t, float> before;
    if (d.brush == Brush::Smooth) {
        const glm::ivec3 a = voxelAt(d.center - reach - S), b = voxelAt(d.center + reach + S);
        for (int z = a.z; z <= b.z; ++z)
            for (int y = a.y; y <= b.y; ++y)
                for (int x = a.x; x <= b.x; ++x) before[(static_cast<int64_t>(x) * 73856093) ^ (static_cast<int64_t>(y) * 19349663) ^ (static_cast<int64_t>(z) * 83492791)] = occAt(x, y, z);
    }
    auto prior = [&](int x, int y, int z) {
        auto it = before.find((static_cast<int64_t>(x) * 73856093) ^ (static_cast<int64_t>(y) * 19349663) ^ (static_cast<int64_t>(z) * 83492791));
        return it != before.end() ? it->second : occAt(x, y, z);
    };
    auto nearSolid = [&](const glm::ivec3& c) {
        for (int k = 0; k < 6; ++k) {
            glm::ivec3 n = c;
            n[k / 2] += (k & 1) ? 1 : -1;
            if (occAt(n.x, n.y, n.z) >= kIso) return true;
        }
        return false;
    };
    auto nearAir = [&](const glm::ivec3& c) {
        for (int k = 0; k < 6; ++k) {
            glm::ivec3 n = c;
            n[k / 2] += (k & 1) ? 1 : -1;
            if (occAt(n.x, n.y, n.z) < kIso) return true;
        }
        return false;
    };

    editVoxels(d.center - reach, d.center + reach, [&](const glm::ivec3& c, const glm::vec3& p, Voxel& v) {
        const float w = sculpt ? soft(p) * shape(p) : shape(p);
        if (w <= 0.0f) return;
        const bool water = v.material == static_cast<uint8_t>(Material::Water);
        const float occ = v.occupancy / 255.0f;
        switch (d.brush) {
            case Brush::Add: {
                if (water && d.material != Material::Water) v = Voxel{}; // solid replaces water
                const float target = w;
                const float next = std::min(1.0f, occ + (target - occ) * std::max(strength, 0.05f) * 2.0f);
                if (next > occ) {
                    if (v.occupancy == 0 || occ < kIso) v.material = static_cast<uint8_t>(d.material);
                    v.occupancy = toOcc(next);
                }
                break;
            }
            case Brush::Subtract: {
                if (water && d.ignoreWater) return;
                const float next = std::max(0.0f, occ - w * std::max(strength, 0.05f) * 2.0f);
                v.occupancy = toOcc(std::min(occ, next));
                break;
            }
            case Brush::Grow: {
                if (water || (!nearSolid(c) && occ <= 0.0f)) return;
                if (v.occupancy == 0) v.material = static_cast<uint8_t>(d.material);
                v.occupancy = toOcc(std::min(1.0f, occ + w * strength * 0.35f));
                break;
            }
            case Brush::Erode: {
                if (water || !nearAir(c)) return;
                v.occupancy = toOcc(std::max(0.0f, occ - w * strength * 0.35f));
                break;
            }
            case Brush::Smooth: {
                if (water && d.ignoreWater) return;
                float sum = 0.0f;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) sum += prior(c.x + dx, c.y + dy, c.z + dz);
                const float avg = sum / 27.0f;
                const float next = occ + (avg - occ) * w * strength;
                if (v.occupancy == 0 && next > 0.02f) {
                    // Take the material of the fullest neighbour.
                    uint8_t best = static_cast<uint8_t>(d.material), bestOcc = 0;
                    for (int k = 0; k < 6; ++k) {
                        glm::ivec3 n = c;
                        n[k / 2] += (k & 1) ? 1 : -1;
                        const Voxel nv = voxel(n.x, n.y, n.z);
                        if (solidMaterial(nv.material) && nv.occupancy > bestOcc) { best = nv.material; bestOcc = nv.occupancy; }
                    }
                    v.material = best;
                }
                v.occupancy = toOcc(std::clamp(next, 0.0f, 1.0f));
                break;
            }
            case Brush::Flatten: {
                if (water && d.ignoreWater) return;
                const float bottom = p.y - S * 0.5f;
                const float target = std::clamp((d.planeHeight - bottom) / S, 0.0f, 1.0f);
                const float next = occ + (target - occ) * w * std::max(strength, 0.1f);
                if (v.occupancy == 0 && next > 0.0f) v.material = static_cast<uint8_t>(d.material);
                v.occupancy = toOcc(next);
                break;
            }
            case Brush::Paint:
                if (v.occupancy > 0 && !water && w * strength > 0.15f) v.material = static_cast<uint8_t>(d.material);
                break;
        }
        if (v.occupancy == 0) v.material = static_cast<uint8_t>(Material::Air);
    });
}

// ============================================================================
//  Generation
// ============================================================================
void TerrainData::generate(const GenerateOptions& o) {
    const glm::ivec3 a = voxelAt(o.center - glm::vec3(o.size.x * 0.5f, 0.0f, o.size.y * 0.5f));
    const glm::ivec3 b = voxelAt(o.center + glm::vec3(o.size.x * 0.5f, 0.0f, o.size.y * 0.5f));
    const int w = b.x - a.x + 1, h = b.z - a.z + 1;
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > 2048u * 2048u) return;
    const float waterY = o.baseHeight + o.waterLevel;
    const uint32_t seed = o.seed * 2654435761u + 17u;

    // Heights per column (world y of the surface).
    std::vector<float> heights(static_cast<size_t>(w) * static_cast<size_t>(h));
    auto at = [&](int x, int z) -> float& { return heights[static_cast<size_t>(z) * static_cast<size_t>(w) + static_cast<size_t>(x)]; };
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            const glm::vec3 p = voxelCenter({ a.x + x, 0, a.z + z });
            // Rolling hills whose valleys dip under the water level.
            const float rolling = std::clamp(fbm2(p.x * 0.010f, p.z * 0.010f, 5, seed) * 1.6f + 0.5f, 0.0f, 1.0f);
            const float hills = rolling * 16.0f * o.hills;
            // Mountain ranges: sharp ridges, only where a broad mask rises.
            const float range = std::clamp((fbm2(p.x * 0.004f + 71.3f, p.z * 0.004f - 27.9f, 3, seed + 13u) * 1.6f - 0.12f) / 0.3f, 0.0f, 1.0f);
            const float mask = range * range * (3.0f - 2.0f * range);
            const float ridge = std::clamp(1.0f - std::abs(fbm2(p.x * 0.008f + 31.7f, p.z * 0.008f - 11.3f, 4, seed + 7u)) * 3.0f, 0.0f, 1.0f);
            const float mountains = ridge * ridge * mask * 64.0f * o.mountains;
            // Fade to the base height near the edges of the area.
            const float ex = std::min(x, w - 1 - x) / std::max(1.0f, w * 0.12f), ez = std::min(z, h - 1 - z) / std::max(1.0f, h * 0.12f);
            const float edge = std::clamp(std::min(ex, ez), 0.0f, 1.0f);
            const float e = edge * edge * (3.0f - 2.0f * edge);
            at(x, z) = o.baseHeight + 0.5f + (hills + mountains) * e;
        }

    const int bottom = voxelAt(glm::vec3(0.0f, o.baseHeight - 6.0f, 0.0f)).y;
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x) {
            const float top = at(x, z);
            // Steepness from the neighbouring columns.
            const float dx = at(std::min(x + 1, w - 1), z) - at(std::max(x - 1, 0), z);
            const float dz = at(x, std::min(z + 1, h - 1)) - at(x, std::max(z - 1, 0));
            const float slope = std::sqrt(dx * dx + dz * dz) / (2.0f * S);
            Material surface = Material::Grass;
            if (o.water && top < waterY + 1.5f) surface = Material::Sand;
            else if (top > o.baseHeight + 48.0f) surface = slope > 1.1f ? Material::Rock : Material::Snow;
            else if (slope > 1.2f) surface = Material::Rock;
            else if (slope > 0.75f) surface = Material::Ground;
            else if (noise3((a.x + x) * 0.11f, 3.3f, (a.z + z) * 0.11f, seed + 3u) > 0.25f) surface = Material::LeafyGrass;
            const int topVoxel = voxelAt(glm::vec3(0.0f, top, 0.0f)).y;
            const int waterVoxel = voxelAt(glm::vec3(0.0f, waterY, 0.0f)).y;
            const int last = std::max(topVoxel, o.water ? waterVoxel : topVoxel);
            for (int y = bottom; y <= last; ++y) {
                const float yBottom = y * S;
                const float fill = std::clamp((top - yBottom) / S, 0.0f, 1.0f);
                Voxel v;
                if (fill > 0.0f) {
                    const float depth = top - (yBottom + S * 0.5f);
                    Material m = surface;
                    if (depth > 8.0f) m = Material::Rock;
                    else if (depth > 2.5f) m = surface == Material::Sand ? Material::Sandstone : surface == Material::Snow ? Material::Rock : Material::Ground;
                    if (o.caves && depth > 6.0f && noise3((a.x + x) * 0.09f, y * 0.13f, (a.z + z) * 0.09f, seed + 9u) > 0.32f) {
                        v = Voxel{}; // a cave
                    } else {
                        v.material = static_cast<uint8_t>(m);
                        v.occupancy = toOcc(fill);
                    }
                }
                if (o.water && v.occupancy < 255 && yBottom < waterY && fill < 1.0f) {
                    // Water above the ground up to the water level.
                    const float wf = std::clamp((waterY - yBottom) / S, 0.0f, 1.0f);
                    if (wf > fill) {
                        v.material = static_cast<uint8_t>(Material::Water);
                        v.occupancy = toOcc(wf);
                    }
                }
                setVoxel(a.x + x, y, a.z + z, v);
            }
        }
    finishEdit();
}

// ============================================================================
//  Queries
// ============================================================================
float TerrainData::density(const glm::vec3& world) const {
    const glm::vec3 p = world / S - 0.5f;
    const glm::ivec3 i(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));
    const glm::vec3 f = p - glm::vec3(i);
    float sum = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        const Voxel v = voxel(i.x + dx, i.y + dy, i.z + dz);
        const float value = solidMaterial(v.material) ? v.occupancy / 255.0f : 0.0f;
        sum += value * (dx ? f.x : 1.0f - f.x) * (dy ? f.y : 1.0f - f.y) * (dz ? f.z : 1.0f - f.z);
    }
    return sum;
}

float TerrainData::waterAt(const glm::vec3& world) const {
    const glm::ivec3 v = voxelAt(world);
    const Voxel x = voxel(v.x, v.y, v.z);
    if (x.material != static_cast<uint8_t>(Material::Water)) return 0.0f;
    // Partly filled: water fills the voxel from the bottom.
    const float level = v.y * S + S * x.occupancy / 255.0f;
    return world.y <= level ? 1.0f : 0.0f;
}

Material TerrainData::materialAt(const glm::vec3& world) const {
    const glm::ivec3 v = voxelAt(world);
    const Voxel x = voxel(v.x, v.y, v.z);
    if (x.occupancy) return static_cast<Material>(x.material);
    // On the surface: the fullest voxel around.
    Voxel best;
    for (int k = 0; k < 6; ++k) {
        glm::ivec3 n = v;
        n[k / 2] += (k & 1) ? 1 : -1;
        const Voxel nv = voxel(n.x, n.y, n.z);
        if (nv.occupancy > best.occupancy && nv.material != static_cast<uint8_t>(Material::Water)) best = nv;
    }
    return static_cast<Material>(best.material);
}

std::optional<TerrainRayHit> TerrainData::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, bool hitWater) const {
    if (chunks_.empty()) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    auto inside = [&](const glm::vec3& p) { return density(p) >= kIso || (hitWater && waterAt(p) > 0.0f); };
    const float step = S * 0.25f;
    float prev = 0.0f;
    for (float t = 0.0f; t <= maxDistance; t += step) {
        const glm::vec3 p = origin + dir * t;
        // Skip empty space a chunk at a time.
        const glm::ivec3 v = voxelAt(p);
        const ChunkKey key = chunkOf(v.x, v.y, v.z);
        bool near = false;
        for (int m = 0; m < 8 && !near; ++m) near = chunk({ key.x - (m & 1), key.y - ((m >> 1) & 1), key.z - ((m >> 2) & 1) }) != nullptr;
        if (!near) { prev = t; continue; }
        if (inside(p)) {
            float lo = prev, hi = t;
            for (int i = 0; i < 10; ++i) {
                const float mid = (lo + hi) * 0.5f;
                if (inside(origin + dir * mid)) hi = mid; else lo = mid;
            }
            TerrainRayHit hit;
            hit.distance = hi;
            hit.position = origin + dir * hi;
            const float e = S * 0.25f;
            const glm::vec3 g(density(hit.position + glm::vec3(e, 0, 0)) - density(hit.position - glm::vec3(e, 0, 0)),
                              density(hit.position + glm::vec3(0, e, 0)) - density(hit.position - glm::vec3(0, e, 0)),
                              density(hit.position + glm::vec3(0, 0, e)) - density(hit.position - glm::vec3(0, 0, e)));
            hit.normal = glm::length(g) > 1e-6f ? -glm::normalize(g) : -dir;
            hit.material = hitWater && waterAt(hit.position) > 0.0f && density(hit.position) < kIso ? Material::Water : materialAt(hit.position + dir * (S * 0.3f));
            return hit;
        }
        prev = t;
    }
    return std::nullopt;
}

// ============================================================================
//  Meshing (surface nets)
// ============================================================================
void TerrainData::meshChunk(const ChunkKey& key, TerrainMesh& solid, TerrainMesh* water) const {
    solid.vertices.clear();
    solid.indices.clear();
    if (water) { water->vertices.clear(); water->indices.clear(); }
    constexpr int P = N + 2; // samples -1..16
    constexpr int C = N + 1; // cells -1..15
    std::vector<float> fs(P * P * P), fw(P * P * P);
    std::vector<uint8_t> mat(P * P * P), occ(P * P * P);
    auto sidx = [](int x, int y, int z) { return ((z + 1) * P + (y + 1)) * P + (x + 1); };
    auto cidx = [](int x, int y, int z) { return ((z + 1) * C + (y + 1)) * C + (x + 1); };
    const int ox = key.x * N, oy = key.y * N, oz = key.z * N;
    bool anySolid = false, anyWater = false;
    for (int z = -1; z <= N; ++z)
        for (int y = -1; y <= N; ++y)
            for (int x = -1; x <= N; ++x) {
                const Voxel v = voxel(ox + x, oy + y, oz + z);
                const int i = sidx(x, y, z);
                mat[i] = v.material;
                occ[i] = v.occupancy;
                fs[i] = solidMaterial(v.material) ? v.occupancy / 255.0f : 0.0f;
                fw[i] = v.material == static_cast<uint8_t>(Material::Water) ? v.occupancy / 255.0f : 0.0f;
                anySolid |= fs[i] >= kIso;
                anyWater |= fw[i] >= kIso;
            }

    static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    const glm::vec3 grassLow = materialColor(Material::Ground).vec();

    auto build = [&](const std::vector<float>& f, TerrainMesh& out, bool isWater) {
        std::vector<int> cellVertex(C * C * C, -1);
        for (int cz = -1; cz < N; ++cz)
            for (int cy = -1; cy < N; ++cy)
                for (int cx = -1; cx < N; ++cx) {
                    float corner[8];
                    int mask = 0;
                    for (int c = 0; c < 8; ++c) {
                        corner[c] = f[sidx(cx + (c & 1), cy + ((c >> 1) & 1), cz + ((c >> 2) & 1))];
                        if (corner[c] >= kIso) mask |= 1 << c;
                    }
                    if (mask == 0 || mask == 255) continue;
                    glm::vec3 sum(0.0f);
                    int n = 0;
                    for (const auto& e : edges) {
                        const float a = corner[e[0]], b = corner[e[1]];
                        if ((a >= kIso) == (b >= kIso)) continue;
                        const float t = std::clamp((kIso - a) / (b - a), 0.0f, 1.0f);
                        const glm::vec3 pa(e[0] & 1, (e[0] >> 1) & 1, (e[0] >> 2) & 1), pb(e[1] & 1, (e[1] >> 1) & 1, (e[1] >> 2) & 1);
                        sum += pa + (pb - pa) * t;
                        ++n;
                    }
                    const glm::vec3 u = sum / static_cast<float>(n);
                    TerrainVertex vtx;
                    vtx.position = (glm::vec3(ox + cx, oy + cy, oz + cz) + u + 0.5f) * S;
                    const glm::vec3 g((corner[1] + corner[3] + corner[5] + corner[7]) - (corner[0] + corner[2] + corner[4] + corner[6]),
                                      (corner[2] + corner[3] + corner[6] + corner[7]) - (corner[0] + corner[1] + corner[4] + corner[5]),
                                      (corner[4] + corner[5] + corner[6] + corner[7]) - (corner[0] + corner[1] + corner[2] + corner[3]));
                    vtx.normal = glm::length(g) > 1e-6f ? -glm::normalize(g) : glm::vec3(0.0f, 1.0f, 0.0f);
                    if (isWater) {
                        vtx.color = glm::vec3(1.0f);
                        vtx.material = static_cast<float>(Material::Water);
                    } else {
                        // Colour: the solid corners' materials, weighted by how full they are.
                        glm::vec3 color(0.0f);
                        float weight = 0.0f;
                        uint8_t main = static_cast<uint8_t>(Material::Rock), mainOcc = 0;
                        for (int c = 0; c < 8; ++c) {
                            const int i = sidx(cx + (c & 1), cy + ((c >> 1) & 1), cz + ((c >> 2) & 1));
                            if (!solidMaterial(mat[i]) || occ[i] == 0) continue;
                            glm::vec3 col = materialColor(static_cast<Material>(mat[i])).vec();
                            // Grass does not grow on steep slopes: earth shows through.
                            if (mat[i] == static_cast<uint8_t>(Material::Grass) || mat[i] == static_cast<uint8_t>(Material::LeafyGrass)) {
                                const float flat = std::clamp((vtx.normal.y - 0.45f) / 0.3f, 0.0f, 1.0f);
                                col = glm::mix(grassLow, col, flat);
                            }
                            const float w = occ[i] / 255.0f;
                            color += col * w;
                            weight += w;
                            if (occ[i] > mainOcc) { mainOcc = occ[i]; main = mat[i]; }
                        }
                        vtx.color = weight > 0.0f ? color / weight : glm::vec3(0.5f);
                        vtx.material = static_cast<float>(main);
                    }
                    cellVertex[cidx(cx, cy, cz)] = static_cast<int>(out.vertices.size());
                    out.vertices.push_back(vtx);
                }

        auto quad = [&](int a, int b, int c, int d) {
            if (a < 0 || b < 0 || c < 0 || d < 0) return;
            const uint32_t i0 = static_cast<uint32_t>(a), i1 = static_cast<uint32_t>(b), i2 = static_cast<uint32_t>(c), i3 = static_cast<uint32_t>(d);
            out.indices.insert(out.indices.end(), { i0, i1, i2, i0, i2, i3 });
        };
        for (int z = 0; z < N; ++z)
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    const bool here = f[sidx(x, y, z)] >= kIso;
                    for (int axis = 0; axis < 3; ++axis) {
                        const int nx = x + (axis == 0), ny = y + (axis == 1), nz = z + (axis == 2);
                        const bool there = f[sidx(nx, ny, nz)] >= kIso;
                        if (here == there) continue;
                        // Water only shows where it meets air (not inside the ground).
                        if (isWater && fs[here ? sidx(nx, ny, nz) : sidx(x, y, z)] >= kIso) continue;
                        int c00, c10, c11, c01;
                        if (axis == 0) {
                            c00 = cellVertex[cidx(x, y - 1, z - 1)]; c10 = cellVertex[cidx(x, y, z - 1)];
                            c11 = cellVertex[cidx(x, y, z)]; c01 = cellVertex[cidx(x, y - 1, z)];
                            if (here) quad(c00, c10, c11, c01); else quad(c00, c01, c11, c10);
                        } else if (axis == 1) {
                            c00 = cellVertex[cidx(x - 1, y, z - 1)]; c10 = cellVertex[cidx(x, y, z - 1)];
                            c11 = cellVertex[cidx(x, y, z)]; c01 = cellVertex[cidx(x - 1, y, z)];
                            if (here) quad(c00, c01, c11, c10); else quad(c00, c10, c11, c01);
                        } else {
                            c00 = cellVertex[cidx(x - 1, y - 1, z)]; c10 = cellVertex[cidx(x, y - 1, z)];
                            c11 = cellVertex[cidx(x, y, z)]; c01 = cellVertex[cidx(x - 1, y, z)];
                            if (here) quad(c00, c10, c11, c01); else quad(c00, c01, c11, c10);
                        }
                    }
                }
        // Vertices of cells outside the chunk's own quads were not used: harmless.
    };
    if (anySolid) build(fs, solid, false);
    if (water && anyWater) build(fw, *water, true);
}

// ============================================================================
//  Colours
// ============================================================================
Color3 TerrainData::defaultMaterialColor(Material m) {
    auto rgb = [](int r, int g, int b) { return Color3(r / 255.0f, g / 255.0f, b / 255.0f); };
    switch (m) {
        case Material::Grass: return rgb(106, 127, 63);
        case Material::LeafyGrass: return rgb(115, 132, 74);
        case Material::Sand: return rgb(143, 126, 95);
        case Material::Sandstone: return rgb(137, 90, 71);
        case Material::Rock: return rgb(102, 108, 111);
        case Material::Slate: return rgb(63, 127, 107);
        case Material::Ground: return rgb(102, 92, 59);
        case Material::Mud: return rgb(58, 46, 36);
        case Material::Snow: return rgb(195, 199, 218);
        case Material::Glacier: return rgb(101, 176, 234);
        case Material::Ice: return rgb(129, 194, 224);
        case Material::Basalt: return rgb(30, 30, 37);
        case Material::Asphalt: return rgb(115, 123, 107);
        case Material::Limestone: return rgb(206, 173, 148);
        case Material::Pavement: return rgb(148, 148, 140);
        case Material::Salt: return rgb(198, 189, 181);
        case Material::CrackedLava: return rgb(232, 156, 74);
        case Material::Concrete: return rgb(127, 102, 63);
        case Material::Brick: return rgb(138, 86, 62);
        case Material::Cobblestone: return rgb(132, 123, 90);
        case Material::WoodPlanks: return rgb(139, 109, 79);
        case Material::Water: return rgb(12, 84, 92);
        default: return rgb(150, 150, 150);
    }
}

bool TerrainData::isTerrainMaterial(Material m) {
    switch (m) {
        case Material::Grass: case Material::LeafyGrass: case Material::Sand: case Material::Sandstone: case Material::Rock:
        case Material::Slate: case Material::Ground: case Material::Mud: case Material::Snow: case Material::Glacier:
        case Material::Ice: case Material::Basalt: case Material::Asphalt: case Material::Limestone: case Material::Pavement:
        case Material::Salt: case Material::CrackedLava: case Material::Concrete: case Material::Brick: case Material::Cobblestone:
        case Material::WoodPlanks: case Material::Water:
            return true;
        default:
            return false;
    }
}

Color3 TerrainData::materialColor(Material m) const {
    const int i = static_cast<int>(m);
    return i >= 0 && i < kMaterialCount ? colors_[static_cast<size_t>(i)] : Color3(0.6f, 0.6f, 0.6f);
}

void TerrainData::setMaterialColor(Material m, const Color3& color) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= kMaterialCount) return;
    colors_[static_cast<size_t>(i)] = color;
    customColor_[static_cast<size_t>(i)] = true;
    for (auto& [key, c] : chunks_) c.revision = g_revision++; // colours are baked into the meshes
    revision_ = g_revision++;
}

// ============================================================================
//  Undo
// ============================================================================
void TerrainData::beginRecording() {
    recording_ = true;
    recorded_.clear();
}

TerrainData::Snapshot TerrainData::endRecording() {
    recording_ = false;
    Snapshot out;
    out.swap(recorded_);
    return out;
}

TerrainData::Snapshot TerrainData::capture(const Snapshot& keys) const {
    Snapshot out;
    for (const auto& [key, unused] : keys) {
        const Chunk* c = chunk(key);
        out[key] = c ? std::optional<Chunk>(*c) : std::nullopt;
    }
    return out;
}

void TerrainData::restore(const Snapshot& snapshot) {
    for (const auto& [key, state] : snapshot) {
        if (state) chunks_[key] = *state;
        else chunks_.erase(key);
        touchAround(key);
    }
    finishEdit();
}

void TerrainData::copyChunk(const ChunkKey& key, const Chunk* source) {
    if (source) {
        Chunk& c = chunks_[key];
        c.voxels = source->voxels;
    } else {
        chunks_.erase(key);
    }
    touchAround(key);
    finishEdit();
}

void TerrainData::copyColors(const TerrainData& other) {
    if (colors_ == other.colors_) return;
    colors_ = other.colors_;
    customColor_ = other.customColor_;
    for (auto& [key, c] : chunks_) c.revision = g_revision++;
    revision_ = g_revision++;
}

// ============================================================================
//  Files
// ============================================================================
namespace {
    template <class T> void put(std::ofstream& out, const T& v) { out.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
    template <class T> bool get(std::ifstream& in, T& v) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(T))); }
}

bool TerrainData::save(const std::filesystem::path& file, std::string* error) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { if (error) *error = "cannot write the terrain file"; return false; }
    out.write("RTER", 4);
    put(out, uint32_t(1));
    put(out, S);
    uint32_t customCount = 0;
    for (bool c : customColor_) customCount += c ? 1 : 0;
    put(out, customCount);
    for (int m = 0; m < kMaterialCount; ++m) {
        if (!customColor_[static_cast<size_t>(m)]) continue;
        put(out, static_cast<uint8_t>(m));
        put(out, colors_[static_cast<size_t>(m)].r);
        put(out, colors_[static_cast<size_t>(m)].g);
        put(out, colors_[static_cast<size_t>(m)].b);
    }
    // Chunks in a stable order (smaller diffs under version control).
    std::vector<ChunkKey> keys;
    for (const auto& [key, c] : chunks_) if (!c.allAir()) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    put(out, static_cast<uint32_t>(keys.size()));
    for (const ChunkKey& key : keys) {
        const Chunk& c = chunks_.at(key);
        put(out, key.x);
        put(out, key.y);
        put(out, key.z);
        // Runs of equal voxels.
        std::vector<std::pair<Voxel, uint16_t>> runs;
        for (const Voxel& v : c.voxels) {
            if (!runs.empty() && runs.back().first == v && runs.back().second < 65535) ++runs.back().second;
            else runs.push_back({ v, uint16_t(1) });
        }
        put(out, static_cast<uint32_t>(runs.size()));
        for (const auto& [v, count] : runs) {
            put(out, v.material);
            put(out, v.occupancy);
            put(out, count);
        }
    }
    if (!out) { if (error) *error = "cannot write the terrain file (disk full?)"; return false; }
    return true;
}

bool TerrainData::load(const std::filesystem::path& file, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return fail("cannot open the terrain file");
    char magic[4];
    uint32_t version = 0, customCount = 0, chunkCount = 0;
    float voxelSize = 0.0f;
    if (!in.read(magic, 4) || std::memcmp(magic, "RTER", 4) != 0 || !get(in, version) || !get(in, voxelSize)) return fail("not a terrain file");
    if (version != 1) return fail("the terrain file is from a newer RIFT");
    ChunkMap loaded;
    std::array<Color3, kMaterialCount> colors;
    std::array<bool, kMaterialCount> custom{};
    for (int m = 0; m < kMaterialCount; ++m) colors[static_cast<size_t>(m)] = defaultMaterialColor(static_cast<Material>(m));
    if (!get(in, customCount) || customCount > 255) return fail("the terrain file is damaged");
    for (uint32_t k = 0; k < customCount; ++k) {
        uint8_t m = 0;
        Color3 c;
        if (!get(in, m) || !get(in, c.r) || !get(in, c.g) || !get(in, c.b)) return fail("the terrain file is damaged");
        if (m < kMaterialCount) { colors[m] = c; custom[m] = true; }
    }
    if (!get(in, chunkCount) || chunkCount > 1000000) return fail("the terrain file is damaged");
    for (uint32_t k = 0; k < chunkCount; ++k) {
        ChunkKey key;
        uint32_t runCount = 0;
        if (!get(in, key.x) || !get(in, key.y) || !get(in, key.z) || !get(in, runCount)) return fail("the terrain file is damaged");
        Chunk& c = loaded[key];
        size_t pos = 0;
        for (uint32_t r = 0; r < runCount; ++r) {
            Voxel v;
            uint16_t count = 0;
            if (!get(in, v.material) || !get(in, v.occupancy) || !get(in, count)) return fail("the terrain file is damaged");
            if (v.material >= kMaterialCount) v.material = static_cast<uint8_t>(Material::Rock);
            if (pos + count > static_cast<size_t>(kChunkVoxels)) return fail("the terrain file is damaged");
            std::fill_n(c.voxels.begin() + static_cast<long>(pos), count, v);
            pos += count;
        }
        c.revision = g_revision++;
    }
    chunks_ = std::move(loaded);
    colors_ = colors;
    customColor_ = custom;
    // Neighbour chunks the meshes need (see finishEdit).
    std::vector<ChunkKey> keys;
    for (const auto& [key, c] : chunks_) keys.push_back(key);
    for (const ChunkKey& key : keys) {
        for (int m = 1; m < 8; ++m) {
            const ChunkKey n{ key.x - (m & 1), key.y - ((m >> 1) & 1), key.z - ((m >> 2) & 1) };
            if (!chunks_.count(n)) chunks_[n].revision = g_revision++;
        }
    }
    revision_ = g_revision++;
    return true;
}

} // namespace rift
