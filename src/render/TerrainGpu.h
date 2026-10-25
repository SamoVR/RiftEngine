#pragma once
//
// TerrainGpu.h -- the terrain's chunk meshes on the GPU. Each frame the
// renderer calls update(): chunks whose revision changed are meshed again
// (a few per frame, so a large terrain appears progressively instead of
// freezing the editor), and chunks of terrains no longer drawn are freed.
//
#include "scene/Terrain.h"

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <unordered_map>

namespace rift {

class TerrainGpu {
public:
    TerrainGpu() = default;
    ~TerrainGpu();
    TerrainGpu(const TerrainGpu&) = delete;
    TerrainGpu& operator=(const TerrainGpu&) = delete;

    // Brings the meshes of `terrain` up to date (at most `budget` chunks meshed).
    void update(const TerrainData& terrain, int budget = 48);
    // Draws the solid / water meshes of the terrain last updated, skipping
    // chunks outside the view. Returns the triangles drawn.
    int drawSolid(const glm::mat4& viewProj);
    int drawWater(const glm::mat4& viewProj);
    bool hasWater() const;
    void clear();

private:
    struct Buffers {
        GLuint vao = 0, vbo = 0, ebo = 0;
        GLsizei count = 0;
        void upload(const TerrainMesh& mesh);
        void release();
    };
    struct Entry {
        Buffers solid, water;
        uint64_t revision = 0;
        glm::vec3 lo{ 0.0f }, hi{ 0.0f };
    };
    struct Terrain {
        std::unordered_map<ChunkKey, Entry, ChunkKeyHash> chunks;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uint64_t, Terrain> terrains_; // by TerrainData::uid()
    uint64_t current_ = 0;
    uint64_t frame_ = 0;

    static bool visible(const glm::mat4& viewProj, const glm::vec3& lo, const glm::vec3& hi);
};

} // namespace rift
