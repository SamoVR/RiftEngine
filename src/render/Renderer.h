#pragma once
//
// Renderer.h -- draws a Scene into a Framebuffer.
//
// Frame: sun shadow map -> opaque parts and terrain -> sky -> editor grid ->
// terrain water -> transparent parts (back to front) -> particles, beams and
// trails -> overlay lines (selection boxes, light and attachment gizmos).
// Lighting comes from the Lighting service (ClockTime moves the sun; sky,
// ambient and fog colours; GlobalShadows) plus up to 16 Point/SpotLights.
//
#include "render/Effects.h"
#include "render/Framebuffer.h"
#include "render/GpuMesh.h"
#include "render/Shader.h"
#include "render/TerrainGpu.h"
#include "render/TextureCache.h"
#include "scene/Scene.h"

#include <chrono>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rift {

struct RenderCamera {
    glm::mat4 view{ 1.0f };
    glm::mat4 projection{ 1.0f };
    glm::vec3 position{ 0.0f };
};

struct RenderSettings {
    bool editor = false;           // grid and light gizmos
    bool grid = true;
    float gridStep = 1.0f;
    bool wireframe = false;
    bool shadows = true;
    int shadowMapSize = 2048;
    float shadowDistance = 140.0f; // studs around the camera that get shadows
    std::unordered_set<uint64_t> selected;
    uint64_t hovered = 0;
    uint64_t hiddenModel = 0;      // not drawn, except the tools in it (the player's own character in first person)
    glm::vec3 selectionColor{ 0.62f, 0.45f, 1.0f };
    glm::vec3 hoverColor{ 0.85f, 0.8f, 1.0f };
    float dt = 0.0f;               // seconds since the last frame: moves particles and trails (0 = frozen)
};

struct RenderStats {
    int parts = 0;
    int drawCalls = 0;
    int triangles = 0;
    int lights = 0;
    int particles = 0;
};

// A SurfaceGui's canvas (drawn into a texture by the host) on a face of a part.
struct SurfaceQuad {
    GLuint texture = 0;                    // premultiplied alpha, canvas top at v = 1
    std::array<glm::vec3, 4> corners{};    // world: top-left, top-right, bottom-right, bottom-left
    bool onTop = false;                    // AlwaysOnTop: not hidden by what is in front
};

// Where the sun is and how bright things are, from Lighting's properties.
struct SunState {
    glm::vec3 direction{ 0.0f, 1.0f, 0.0f }; // towards the sun (or moon at night)
    glm::vec3 color{ 1.0f };                 // linear, includes brightness
    glm::vec3 skyColor{ 0.4f }, horizonColor{ 0.6f }, ambient{ 0.3f }, groundColor{ 0.2f }, fogColor{ 0.6f };
    float day = 1.0f;                        // 0 night .. 1 day
    glm::vec3 moonDirection{ 0.0f, 1.0f, 0.0f }; // towards the moon (opposite the sun)
};
SunState computeSun(const Instance& lighting);

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Compiles the shaders and uploads the primitives. Needs a GL context.
    bool init(std::string* error = nullptr);

    void render(const Scene& scene, const RenderCamera& camera, const RenderSettings& settings, Framebuffer& target);

    // Overlay lines for the next render() (gizmo previews, debug drawing).
    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, bool onTop = false);
    void wireBox(const glm::mat4& world, const glm::vec4& color, bool onTop = false); // unit cube transformed
    // SurfaceGuis for the next render().
    void setSurfaceGuis(std::vector<SurfaceQuad> quads) { surfaces_ = std::move(quads); }

    TextureCache& textures() { return textures_; }
    const RenderStats& stats() const { return stats_; }
    const GpuMesh& primitive(PartShape shape) const;
    // The mesh a part is drawn with: its shape, or (MeshPart) its model file,
    // loaded on first use. A model that cannot be loaded draws as a block.
    const GpuMesh& meshFor(const Instance& part);
    // Forget loaded models (project switch, a model file changed).
    void clearModels() { models_.clear(); }

private:
    struct DrawItem {
        const Instance* part;
        glm::mat4 world;
        float distance;
    };
    struct LineVertex { glm::vec3 pos; glm::vec4 color; };

    std::unique_ptr<Shader> partShader_, shadowShader_, skyShader_, gridShader_, lineShader_, terrainShader_, waterShader_, effectShader_;
    TerrainGpu terrain_;
    Effects effects_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::array<std::unique_ptr<GpuMesh>, 5> primitives_;
    std::unordered_map<std::string, std::unique_ptr<GpuMesh>> models_; // null: failed to load
    ShadowMap shadowMap_;
    TextureCache textures_;
    GLuint emptyVao_ = 0, gridVao_ = 0, gridVbo_ = 0, lineVao_ = 0, lineVbo_ = 0;
    std::vector<LineVertex> lines_, linesOnTop_;
    std::vector<SurfaceQuad> surfaces_;
    GLuint surfaceVao_ = 0, surfaceVbo_ = 0;
    void drawSurfaces(bool onTop, const glm::mat4& viewProj, const glm::vec3& tint);
    RenderStats stats_;

    void collect(const Scene& scene, const glm::vec3& cameraPos, uint64_t hidden, std::vector<DrawItem>& opaque, std::vector<DrawItem>& transparent,
                 std::vector<const Instance*>& lights, std::vector<glm::mat4>& lightFrames) const;
    void drawParts(const std::vector<DrawItem>& items, const RenderSettings& settings, const glm::mat4& viewProj);
    void drawLines(const std::vector<LineVertex>& lines, const glm::mat4& viewProj);
    void addEditorOverlays(const Scene& scene, const RenderSettings& settings, const std::vector<const Instance*>& lights,
                           const std::vector<glm::mat4>& lightFrames);
};

} // namespace rift
