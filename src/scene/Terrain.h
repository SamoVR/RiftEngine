#pragma once
//
// Terrain.h -- Roblox-style smooth voxel terrain (workspace.Terrain).
//
// The world is a grid of 2-stud voxels; each has a material (Grass, Rock,
// Water, ... or Air) and an occupancy (how full it is, 0..255). Voxels are
// stored in 16x16x16 chunks that exist only where something was ever placed.
// The surface is meshed per chunk with surface nets, so terrain looks smooth
// and occupancy below 255 rounds shapes off. Water voxels are meshed apart
// (a see-through surface) and never collide.
//
// Everything here is plain CPU code (no OpenGL): the renderer and physics
// build their own objects from meshChunk(), and compare chunk revisions to
// know what changed.
//
#include "scene/Instance.h"

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rift {

struct Voxel {
    uint8_t material = static_cast<uint8_t>(Material::Air);
    uint8_t occupancy = 0;
    bool operator==(const Voxel& o) const { return material == o.material && occupancy == o.occupancy; }
};

struct ChunkKey {
    int x = 0, y = 0, z = 0;
    bool operator==(const ChunkKey& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator<(const ChunkKey& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
};
struct ChunkKeyHash {
    size_t operator()(const ChunkKey& k) const {
        return static_cast<size_t>(k.x) * 73856093u ^ static_cast<size_t>(k.y) * 19349663u ^ static_cast<size_t>(k.z) * 83492791u;
    }
};

struct TerrainVertex {
    glm::vec3 position;  // world space
    glm::vec3 normal;
    glm::vec3 color;     // sRGB material colour (blended between voxels)
    float material;      // the main Material (index), for detail patterns
};
struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    bool empty() const { return indices.empty(); }
};

struct TerrainRayHit {
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
    float distance = 0.0f;
    Material material = Material::Air;
};

class TerrainData {
public:
    static constexpr float kVoxelSize = 2.0f; // studs
    static constexpr int kChunkSize = 16;     // voxels per side
    static constexpr int kChunkVoxels = kChunkSize * kChunkSize * kChunkSize;

    struct Chunk {
        std::array<Voxel, kChunkVoxels> voxels{};
        uint64_t revision = 0; // changes whenever this chunk (or a neighbour it meshes with) changes
        bool allAir() const;
    };
    using ChunkMap = std::unordered_map<ChunkKey, Chunk, ChunkKeyHash>;

    TerrainData();
    TerrainData(const TerrainData& other);
    TerrainData& operator=(const TerrainData& other);

    uint64_t uid() const { return uid_; }           // unique per TerrainData object (render caches)
    uint64_t revision() const { return revision_; } // increases with every change
    bool empty() const;
    const ChunkMap& chunks() const { return chunks_; }

    // ---- voxels (voxel coordinates: voxel (i,j,k) spans [i,i+1) * kVoxelSize)
    Voxel voxel(int x, int y, int z) const;
    void setVoxel(int x, int y, int z, const Voxel& v);
    static glm::ivec3 voxelAt(const glm::vec3& world);
    static glm::vec3 voxelCenter(const glm::ivec3& v);
    static ChunkKey chunkOf(int x, int y, int z);

    // ---- Roblox's Terrain API (frames are world transforms without scale)
    void fillBall(const glm::vec3& center, float radius, Material material);
    void fillBlock(const glm::mat4& frame, const glm::vec3& size, Material material);
    void fillCylinder(const glm::mat4& frame, float height, float radius, Material material); // axis = frame's Y
    void clear();

    // ---- editor brushes (one call = one dab; strength 0..1)
    enum class Brush { Add, Subtract, Grow, Erode, Smooth, Flatten, Paint };
    struct Dab {
        Brush brush = Brush::Add;
        glm::vec3 center{ 0.0f };
        float radius = 4.0f;
        bool cube = false;
        float strength = 0.5f;
        Material material = Material::Grass;
        float planeHeight = 0.0f; // Flatten: the surface height to level to
        bool ignoreWater = true;  // Subtract/Erode/Smooth leave water alone
    };
    void applyDab(const Dab& dab);

    // ---- procedural generation (Terrain Editor > Generate)
    struct GenerateOptions {
        glm::vec3 center{ 0.0f };
        glm::vec2 size{ 256.0f, 256.0f }; // studs
        float baseHeight = 0.0f;          // studs: the lowest ground
        float hills = 1.0f;               // 0..2: rolling hills
        float mountains = 0.6f;           // 0..2: ridges and peaks
        bool water = true;
        float waterLevel = 6.0f;          // studs above baseHeight
        bool caves = false;
        uint32_t seed = 1;
    };
    void generate(const GenerateOptions& options);

    // ---- queries
    float density(const glm::vec3& world) const; // 0..1 solid (water excluded), trilinear between voxel centres
    float waterAt(const glm::vec3& world) const; // 0..1
    Material materialAt(const glm::vec3& world) const;
    std::optional<TerrainRayHit> raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, bool hitWater = false) const;

    // ---- meshes (world space) of one chunk's solid surface and water surface
    void meshChunk(const ChunkKey& key, TerrainMesh& solid, TerrainMesh* water) const;

    // ---- material colours (Terrain:SetMaterialColor)
    Color3 materialColor(Material material) const;
    void setMaterialColor(Material material, const Color3& color);
    static Color3 defaultMaterialColor(Material material);
    static bool isTerrainMaterial(Material material); // usable on terrain (not plastic, neon, ...)

    // ---- undo: the chunks as they were before the edits since beginRecording()
    using Snapshot = std::map<ChunkKey, std::optional<Chunk>>; // nullopt: did not exist
    void beginRecording();
    Snapshot endRecording();          // the "before" states
    Snapshot capture(const Snapshot& keys) const; // the current state of the same chunks
    void restore(const Snapshot& snapshot);

    // ---- replication: make chunk `key` equal to `source` (nullptr: remove it), copy colours
    void copyChunk(const ChunkKey& key, const Chunk* source);
    void copyColors(const TerrainData& other);

    // ---- files: <scene folder>/terrain.bin
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    bool load(const std::filesystem::path& file, std::string* error = nullptr);

private:
    uint64_t uid_;
    uint64_t revision_ = 0;
    ChunkMap chunks_;
    std::array<Color3, kMaterialCount> colors_;
    std::array<bool, kMaterialCount> customColor_{};
    bool recording_ = false;
    Snapshot recorded_;
    // Chunks whose meshes the current edit changed: the voxel's own chunk, and
    // the neighbours that read it along their faces (a mesh uses one voxel
    // beyond each side). lastTouched_ skips the set for interior voxels.
    std::unordered_set<ChunkKey, ChunkKeyHash> touched_;
    std::optional<ChunkKey> lastTouched_;

    Chunk* chunk(const ChunkKey& key, bool create);
    const Chunk* chunk(const ChunkKey& key) const;
    void touchAround(const ChunkKey& key); // a whole chunk changed: it and its 26 neighbours
    void finishEdit();               // revisions, neighbour chunks
    void editVoxels(const glm::vec3& lo, const glm::vec3& hi, const std::function<void(const glm::ivec3&, const glm::vec3&, Voxel&)>& f);
};

} // namespace rift
