#pragma once
//
// Effects.h -- ParticleEmitters, Beams and Trails.
//
// They are visual only: each scene that is drawn gets its own simulation
// (particles, trail points) kept here by instance id, advanced by the frame
// time the renderer is given, and turned into camera-facing quads and
// ribbons for one transparent pass. Nothing is replicated: every client
// simulates what it sees (as in Roblox), and ParticleEmitter:Emit / :Clear
// arrive as counters on the instance (Instance::emitRequests / clearRequests).
//
#include "scene/Scene.h"

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class Shader;

namespace rift {

class TextureCache;

class Effects {
public:
    Effects() = default;
    ~Effects();
    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;

    // Advances the effects of `scene` by `dt` seconds (0 = frozen) and builds
    // this frame's geometry, sorted back to front. `sceneLight` is roughly how
    // bright the world is (LightInfluence darkens towards it).
    void update(const Scene& scene, float dt, const glm::vec3& cameraPos, const glm::vec3& sceneLight);
    // Draws what update() built (depth test on, depth writes off, premultiplied blending).
    void draw(Shader& shader, TextureCache& textures, const glm::mat4& viewProj);
    int particleCount() const { return particles_; }
    size_t vertexCount() const { return vertices_.size(); }
    void clear();

    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;
        glm::vec4 color;   // sRGB colour * brightness, alpha = opacity
        float emission;    // LightEmission: 0 blended, 1 additive
        float mode;        // 0 textured, 1 soft dot (no texture), 2 solid band (no texture)
    };

private:
    struct Particle {
        glm::vec3 position, velocity; // world space, or the emitter's space when LockedToPart
        float age = 0.0f, life = 1.0f;
        float rotation = 0.0f, spin = 0.0f;
        float sizeNoise = 0.0f, fadeNoise = 0.0f; // -1..1, scaled by the sequences' envelopes
    };
    struct Emitter {
        std::vector<Particle> particles;
        float pending = 0.0f;            // fractional particles owed by Rate
        int emitSeen = 0;                // Instance::emitRequests already handled
        uint32_t clearSeen = 0;
        uint64_t frame = 0;
        std::mt19937 rng{ 12345u };
    };
    struct TrailPoint { glm::vec3 a, b; float time; };
    struct Trail {
        std::deque<TrailPoint> points;   // oldest first
        float time = 0.0f;
        glm::vec3 lastMid{ 0.0f };       // where the last point was added
        bool started = false;
        uint64_t frame = 0;
    };
    struct SceneState {
        std::unordered_map<uint64_t, Emitter> emitters;
        std::unordered_map<uint64_t, Trail> trails;
        uint64_t frame = 0;
        float time = 0.0f;               // for scrolling beam textures
    };
    struct Batch {
        std::string texture;
        size_t first = 0, count = 0;     // vertices
        float depth = 0.0f;              // distance to the camera (sorting)
    };

    std::unordered_map<const Scene*, SceneState> scenes_;
    std::vector<Vertex> vertices_;
    std::vector<Batch> batches_;
    uint64_t frame_ = 0;
    int particles_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    size_t capacity_ = 0;

    void simulate(const Instance& emitter, Emitter& state, float dt);
    void buildParticles(const Instance& emitter, const Emitter& state, const glm::vec3& cameraPos, const glm::vec3& sceneLight);
    void buildBeam(const Scene& scene, const Instance& beam, float time, const glm::vec3& cameraPos, const glm::vec3& sceneLight);
    void buildTrail(const Scene& scene, const Instance& trail, Trail& state, float dt, const glm::vec3& cameraPos, const glm::vec3& sceneLight);
    void quad(const glm::vec3 corners[4], const glm::vec2 uvs[4], const glm::vec4 colors[4], float emission, float mode);
};

} // namespace rift
