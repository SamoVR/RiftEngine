#pragma once
//
// ModelLoader.h -- reads 3D model files for MeshParts (no OpenGL here, so
// tests and the editor's import use it too).
//
//   .obj         Wavefront OBJ (+ .mtl colour and texture)
//   .gltf / .glb glTF 2.0: every mesh of the default scene, node transforms
//                applied; base colour factor and texture
//
// All meshes of a file are merged into one, centred, and scaled to fit the
// unit cube [-0.5, 0.5]^3 like the built-in shapes, so a MeshPart's Size
// stretches the model to that size (as in Roblox). `nativeSize` keeps the
// model's own dimensions (1 unit = 1 stud) for the initial Size.
//
#include "render/Primitives.h"

#include <filesystem>
#include <string>

namespace rift {

struct ModelData {
    MeshData mesh;                 // unit-cube normalised
    glm::vec3 nativeSize{ 1.0f };  // size before normalising
    bool hasColor = false;
    glm::vec3 color{ 1.0f };       // first material's base colour (sRGB 0..1)
    std::filesystem::path texture; // first material's colour texture, if any (absolute)
    int meshCount = 0;             // meshes / objects merged
};

bool loadModel(const std::filesystem::path& file, ModelData& out, std::string* error = nullptr);

} // namespace rift
