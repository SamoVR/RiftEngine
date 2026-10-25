#include "render/TerrainGpu.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace rift {

TerrainGpu::~TerrainGpu() { clear(); }

void TerrainGpu::Buffers::upload(const TerrainMesh& mesh) {
    if (mesh.empty()) { release(); return; }
    if (!vao) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), reinterpret_cast<void*>(offsetof(TerrainVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), reinterpret_cast<void*>(offsetof(TerrainVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), reinterpret_cast<void*>(offsetof(TerrainVertex, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), reinterpret_cast<void*>(offsetof(TerrainVertex, material)));
    } else {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    }
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(TerrainVertex)), mesh.vertices.data(), GL_STATIC_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(uint32_t)), mesh.indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    count = static_cast<GLsizei>(mesh.indices.size());
}

void TerrainGpu::Buffers::release() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    vao = vbo = ebo = 0;
    count = 0;
}

void TerrainGpu::clear() {
    for (auto& [uid, t] : terrains_) {
        for (auto& [key, e] : t.chunks) { e.solid.release(); e.water.release(); }
    }
    terrains_.clear();
}

void TerrainGpu::update(const TerrainData& terrain, int budget) {
    ++frame_;
    current_ = terrain.uid();
    Terrain& t = terrains_[current_];
    t.lastFrame = frame_;

    // Chunks that are gone.
    for (auto it = t.chunks.begin(); it != t.chunks.end();) {
        if (!terrain.chunks().count(it->first)) {
            it->second.solid.release();
            it->second.water.release();
            it = t.chunks.erase(it);
        } else {
            ++it;
        }
    }
    // Chunks that changed (or are new).
    TerrainMesh solid, water;
    for (const auto& [key, chunk] : terrain.chunks()) {
        if (budget <= 0) break;
        auto it = t.chunks.find(key);
        if (it != t.chunks.end() && it->second.revision == chunk.revision) continue;
        Entry& e = t.chunks[key];
        terrain.meshChunk(key, solid, &water);
        e.solid.upload(solid);
        e.water.upload(water);
        e.revision = chunk.revision;
        const float size = TerrainData::kChunkSize * TerrainData::kVoxelSize;
        e.lo = glm::vec3(key.x, key.y, key.z) * size - glm::vec3(TerrainData::kVoxelSize * 2.0f);
        e.hi = e.lo + glm::vec3(size + TerrainData::kVoxelSize * 4.0f);
        --budget;
    }

    // Terrains not drawn for a while (an old Play session): free them.
    for (auto it = terrains_.begin(); it != terrains_.end();) {
        if (it->first != current_ && frame_ - it->second.lastFrame > 120) {
            for (auto& [key, e] : it->second.chunks) { e.solid.release(); e.water.release(); }
            it = terrains_.erase(it);
        } else {
            ++it;
        }
    }
}

bool TerrainGpu::visible(const glm::mat4& m, const glm::vec3& lo, const glm::vec3& hi) {
    // Outside if all eight corners are beyond one clip plane.
    glm::vec4 c[8];
    for (int i = 0; i < 8; ++i) c[i] = m * glm::vec4((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z, 1.0f);
    for (int axis = 0; axis < 3; ++axis) {
        bool allBelow = true, allAbove = true;
        for (const glm::vec4& p : c) {
            allBelow &= p[axis] < -p.w;
            allAbove &= p[axis] > p.w;
        }
        if (allBelow || allAbove) return false;
    }
    return true;
}

int TerrainGpu::drawSolid(const glm::mat4& viewProj) {
    auto it = terrains_.find(current_);
    if (it == terrains_.end()) return 0;
    int triangles = 0;
    for (auto& [key, e] : it->second.chunks) {
        if (!e.solid.count || !visible(viewProj, e.lo, e.hi)) continue;
        glBindVertexArray(e.solid.vao);
        glDrawElements(GL_TRIANGLES, e.solid.count, GL_UNSIGNED_INT, nullptr);
        triangles += e.solid.count / 3;
    }
    glBindVertexArray(0);
    return triangles;
}

int TerrainGpu::drawWater(const glm::mat4& viewProj) {
    auto it = terrains_.find(current_);
    if (it == terrains_.end()) return 0;
    int triangles = 0;
    for (auto& [key, e] : it->second.chunks) {
        if (!e.water.count || !visible(viewProj, e.lo, e.hi)) continue;
        glBindVertexArray(e.water.vao);
        glDrawElements(GL_TRIANGLES, e.water.count, GL_UNSIGNED_INT, nullptr);
        triangles += e.water.count / 3;
    }
    glBindVertexArray(0);
    return triangles;
}

bool TerrainGpu::hasWater() const {
    auto it = terrains_.find(current_);
    if (it == terrains_.end()) return false;
    for (const auto& [key, e] : it->second.chunks) if (e.water.count) return true;
    return false;
}

} // namespace rift
