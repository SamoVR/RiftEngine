#pragma once
//
// GpuMesh.h -- an indexed triangle mesh in GPU memory (VAO + VBO + EBO).
// Shared by every part that uses it (all blocks share one), so cloning a
// scene for Play or duplicating parts never uploads geometry again.
//
#include "render/Primitives.h"

#include <GL/glew.h>
#include <memory>

namespace rift {

class GpuMesh {
public:
    explicit GpuMesh(const MeshData& data);
    ~GpuMesh();
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    void draw() const;
    const glm::vec3& boundsMin() const { return boundsMin_; }
    const glm::vec3& boundsMax() const { return boundsMax_; }
    size_t triangleCount() const { return indexCount_ / 3; }

private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLsizei indexCount_ = 0;
    glm::vec3 boundsMin_{ 0.0f }, boundsMax_{ 0.0f };
};

} // namespace rift
