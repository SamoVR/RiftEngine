#include "render/Renderer.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "render/ModelLoader.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>

namespace rift {

namespace {
    constexpr int kMaxLights = 16;
    constexpr float kPi = 3.14159265358979f;

    glm::vec3 toLinear(const Color3& c) { return glm::pow(glm::max(c.vec(), glm::vec3(0.0f)), glm::vec3(2.2f)); }

    float smoothstep(float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}

// ============================================================================
//  Sun / sky from the Lighting service
// ============================================================================
SunState computeSun(const Instance& L) {
    SunState s;
    // 6:00 sunrise in the east (+X), 12:00 overhead (tilted a little south so
    // shadows are never perfectly axis-aligned), 18:00 sunset in the west.
    const float a = (L.clockTime - 6.0f) / 12.0f * kPi;
    const glm::vec3 sun = glm::normalize(glm::vec3(std::cos(a) * 0.9f, std::sin(a), 0.35f));
    s.day = smoothstep(-0.12f, 0.18f, sun.y);
    const float sunset = 1.0f - smoothstep(0.0f, 0.45f, std::abs(sun.y)); // warm light near the horizon

    const glm::vec3 sunTint = glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.55f, 0.3f), sunset * 0.8f);
    if (sun.y > -0.05f) {
        s.direction = sun.y < 0.02f ? glm::normalize(glm::vec3(sun.x, 0.02f, sun.z)) : sun;
        s.color = toLinear(L.color) * sunTint * std::max(0.0f, L.brightness) * 1.3f * s.day;
    } else {
        // Moonlight from the opposite side of the sky.
        s.direction = glm::normalize(glm::vec3(-sun.x, std::max(0.25f, -sun.y), -sun.z));
        s.color = glm::vec3(0.35f, 0.45f, 0.75f) * 0.18f * std::max(0.0f, L.brightness);
    }

    const glm::vec3 nightSky(0.006f, 0.009f, 0.025f);
    s.skyColor = glm::mix(nightSky, toLinear(L.skyColor), s.day);
    s.horizonColor = glm::mix(nightSky * 2.0f, toLinear(L.horizonColor), s.day);
    s.horizonColor = glm::mix(s.horizonColor, glm::vec3(1.0f, 0.45f, 0.2f) * 0.8f, sunset * s.day * 0.6f);
    s.ambient = toLinear(L.ambient) * glm::mix(0.18f, 1.0f, s.day);
    s.groundColor = s.ambient * 0.5f;
    s.fogColor = glm::mix(nightSky * 2.0f, toLinear(L.fogColor), s.day);
    return s;
}

// ============================================================================
//  Setup
// ============================================================================
Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (emptyVao_) glDeleteVertexArrays(1, &emptyVao_);
    if (gridVao_) glDeleteVertexArrays(1, &gridVao_);
    if (gridVbo_) glDeleteBuffers(1, &gridVbo_);
    if (lineVao_) glDeleteVertexArrays(1, &lineVao_);
    if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
}

bool Renderer::init(std::string* error) {
    auto load = [&](std::unique_ptr<Shader>& out, const char* vert, const char* frag) {
        out = std::make_unique<Shader>(Paths::shader(vert), Paths::shader(frag));
        if (!out->isValid()) {
            if (error) *error = std::string("shader ") + vert + " / " + frag + " failed to compile (see the log)";
            return false;
        }
        return true;
    };
    if (!load(partShader_, "part.vert", "part.frag") || !load(shadowShader_, "shadow.vert", "shadow.frag")
        || !load(skyShader_, "sky.vert", "sky.frag") || !load(gridShader_, "grid.vert", "grid.frag")
        || !load(lineShader_, "line.vert", "line.frag") || !load(terrainShader_, "terrain.vert", "terrain.frag")
        || !load(waterShader_, "terrain.vert", "water.frag") || !load(effectShader_, "effect.vert", "effect.frag")) {
        return false;
    }
    for (int i = 0; i < 5; ++i) primitives_[i] = std::make_unique<GpuMesh>(primitiveMesh(static_cast<PartShape>(i)));

    glGenVertexArrays(1, &emptyVao_);

    const float quad[] = { -1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, -1, 1, 0, 1, -1, 0, 1 };
    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindVertexArray(gridVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, color)));
    glBindVertexArray(0);
    return true;
}

const GpuMesh& Renderer::primitive(PartShape shape) const {
    const int i = static_cast<int>(shape);
    return *primitives_[i >= 0 && i < 5 ? i : 0];
}

const GpuMesh& Renderer::meshFor(const Instance& p) {
    if (p.cls == ClassId::MeshPart && !p.mesh.path.empty()) {
        auto it = models_.find(p.mesh.path);
        if (it == models_.end()) {
            ModelData model;
            std::string error;
            std::unique_ptr<GpuMesh> gpu;
            if (loadModel(Paths::resolveAsset(p.mesh.path), model, &error)) gpu = std::make_unique<GpuMesh>(model.mesh);
            else LOG_WARN("Renderer", "Cannot load the model " + p.mesh.path + ": " + error);
            it = models_.emplace(p.mesh.path, std::move(gpu)).first;
        }
        if (it->second) return *it->second;
    }
    return primitive(p.cls == ClassId::Part ? p.shape : PartShape::Block);
}

// ============================================================================
//  Overlay lines
// ============================================================================
void Renderer::line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, bool onTop) {
    auto& list = onTop ? linesOnTop_ : lines_;
    list.push_back({ a, color });
    list.push_back({ b, color });
}

void Renderer::wireBox(const glm::mat4& world, const glm::vec4& color, bool onTop) {
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        c[i] = glm::vec3(world * glm::vec4((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f, 1.0f));
    }
    static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (const auto& e : edges) line(c[e[0]], c[e[1]], color, onTop);
}

void Renderer::drawLines(const std::vector<LineVertex>& lines, const glm::mat4& viewProj) {
    if (lines.empty()) return;
    lineShader_->use();
    lineShader_->setMat4("uViewProj", viewProj);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lines.size() * sizeof(LineVertex)), lines.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lines.size()));
    ++stats_.drawCalls;
}

// ============================================================================
//  Scene traversal
// ============================================================================
void Renderer::collect(const Scene& scene, const glm::vec3& cameraPos, std::vector<DrawItem>& opaque, std::vector<DrawItem>& transparent,
                       std::vector<const Instance*>& lights, std::vector<glm::mat4>& lightFrames) const {
    // One pass down the tree with the parent frame carried along (no
    // repeated walks up the ancestor chain).
    struct Node { const Instance* inst; glm::mat4 frame; };
    std::vector<Node> stack;
    for (auto it = scene.workspace().children.rbegin(); it != scene.workspace().children.rend(); ++it) stack.push_back({ *it, glm::mat4(1.0f) });
    while (!stack.empty()) {
        const Node n = stack.back();
        stack.pop_back();
        const Instance& i = *n.inst;
        const glm::mat4 frame = i.hasTransform() ? n.frame * i.localFrame() : n.frame;
        if (i.isBasePart()) {
            if (i.visible && i.transparency < 0.999f) {
                const glm::mat4 world = frame * glm::scale(glm::mat4(1.0f), i.size);
                const float distance = glm::length(glm::vec3(frame[3]) - cameraPos);
                const bool isTransparent = i.transparency > 0.001f || i.material == Material::Glass || i.material == Material::ForceField
                    || i.material == Material::Ice || i.material == Material::Water;
                (isTransparent ? transparent : opaque).push_back({ &i, world, distance });
            }
        } else if (i.isLight() && i.enabled) {
            lights.push_back(&i);
            lightFrames.push_back(frame);
        }
        for (auto it = i.children.rbegin(); it != i.children.rend(); ++it) stack.push_back({ *it, frame });
    }
}

void Renderer::drawParts(const std::vector<DrawItem>& items, const RenderSettings& settings) {
    for (const DrawItem& d : items) {
        const Instance& p = *d.part;
        partShader_->setMat4("uModel", d.world);
        partShader_->setMat3("uNormalMatrix", glm::transpose(glm::inverse(glm::mat3(d.world))));
        partShader_->setVec3("uColor", p.color.vec());
        partShader_->setFloat("uTransparency", p.transparency);
        partShader_->setInt("uMaterial", static_cast<int>(p.material));
        partShader_->setFloat("uReflectance", p.reflectance);
        partShader_->setVec3("uSize", p.size);
        std::shared_ptr<Texture> tex = p.texture.path.empty() ? nullptr : textures_.get(p.texture.path);
        if (tex) {
            tex->bind(0);
            partShader_->setInt("uHasTexture", 1);
            partShader_->setVec2("uTextureScale", p.textureScale);
        } else {
            partShader_->setInt("uHasTexture", 0);
        }
        float tint = 0.0f;
        if (settings.editor && settings.hovered == p.id && !settings.selected.count(p.id)) tint = 0.12f;
        partShader_->setFloat("uTintAmount", tint);
        const GpuMesh& mesh = meshFor(p);
        mesh.draw();
        ++stats_.drawCalls;
        stats_.triangles += static_cast<int>(mesh.triangleCount());
    }
}

// ============================================================================
//  Editor overlays
// ============================================================================
void Renderer::addEditorOverlays(const Scene& scene, const RenderSettings& settings, const std::vector<const Instance*>& lights,
                                 const std::vector<glm::mat4>& lightFrames) {
    // Selection and hover boxes. Hidden behind geometry they are drawn faintly
    // (onTop pass), so a selected part inside a wall is still findable.
    auto boxFor = [&](const Instance& i, const glm::vec4& color) {
        if (i.isBasePart()) {
            const glm::mat4 world = i.worldFrame() * glm::scale(glm::mat4(1.0f), i.size + glm::vec3(0.06f));
            wireBox(world, color);
            wireBox(world, glm::vec4(glm::vec3(color), color.a * 0.3f), true);
        } else if (i.cls == ClassId::Model || i.isService()) {
            glm::vec3 lo(1e30f), hi(-1e30f);
            Scene::forEachDescendant(const_cast<Instance&>(i), [&](Instance& d) {
                if (!d.isBasePart()) return;
                const glm::mat4 w = d.worldTransform();
                for (int c = 0; c < 8; ++c) {
                    const glm::vec3 p = glm::vec3(w * glm::vec4((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f, 1.0f));
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
            });
            if (lo.x > hi.x || i.isService()) return;
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), (lo + hi) * 0.5f) * glm::scale(glm::mat4(1.0f), hi - lo + glm::vec3(0.1f));
            wireBox(world, color);
        }
    };
    for (uint64_t id : settings.selected) {
        if (const Instance* i = scene.find(id)) boxFor(*i, glm::vec4(settings.selectionColor, 1.0f));
    }
    if (settings.hovered && !settings.selected.count(settings.hovered)) {
        if (const Instance* i = scene.find(settings.hovered)) boxFor(*i, glm::vec4(settings.hoverColor, 0.55f));
    }

    // Light gizmos: three rings for point lights, a cone for spot lights.
    for (size_t k = 0; k < lights.size(); ++k) {
        const Instance& l = *lights[k];
        const glm::mat4& f = lightFrames[k];
        const glm::vec3 pos(f[3]);
        const bool selected = settings.selected.count(l.id) != 0;
        const glm::vec4 color(glm::mix(l.color.vec(), glm::vec3(1.0f), 0.3f), selected ? 1.0f : 0.6f);
        const float r = 0.35f;
        for (int axis = 0; axis < 3; ++axis) {
            glm::vec3 prev;
            for (int s = 0; s <= 24; ++s) {
                const float t = s / 24.0f * 2.0f * kPi;
                glm::vec3 offset(0.0f);
                offset[(axis + 1) % 3] = std::cos(t) * r;
                offset[(axis + 2) % 3] = std::sin(t) * r;
                const glm::vec3 p = pos + offset;
                if (s > 0) line(prev, p, color);
                prev = p;
            }
        }
        if (l.cls == ClassId::SpotLight) {
            const glm::vec3 dir = -glm::normalize(glm::vec3(f[2]));
            const glm::vec3 right = glm::normalize(glm::vec3(f[0])), up = glm::normalize(glm::vec3(f[1]));
            const float length = std::min(l.range, 6.0f);
            const float radius = std::tan(glm::radians(l.angle * 0.5f)) * length;
            for (int s = 0; s < 8; ++s) {
                const float t = s / 8.0f * 2.0f * kPi;
                line(pos, pos + dir * length + (right * std::cos(t) + up * std::sin(t)) * radius, color);
            }
        }
        if (selected) {
            // Range of the light, as a faint ring on the ground plane through it.
            glm::vec3 prev;
            for (int s = 0; s <= 48; ++s) {
                const float t = s / 48.0f * 2.0f * kPi;
                const glm::vec3 p = pos + glm::vec3(std::cos(t), 0.0f, std::sin(t)) * l.range;
                if (s > 0) line(prev, p, glm::vec4(glm::vec3(color), 0.35f));
                prev = p;
            }
        }
    }

    // Attachments: a small diamond with its axes, shown when it (or its part,
    // or a beam / trail / emitter using it) is selected, or when Visible is on.
    std::unordered_set<uint64_t> shown;
    for (uint64_t id : settings.selected) {
        const Instance* i = scene.find(id);
        if (!i) continue;
        if (i->cls == ClassId::Attachment) shown.insert(i->id);
        if (i->cls == ClassId::Beam || i->cls == ClassId::Trail) { shown.insert(i->attachment0.id); shown.insert(i->attachment1.id); }
        if (i->cls == ClassId::ParticleEmitter && i->parent && i->parent->cls == ClassId::Attachment) shown.insert(i->parent->id);
        for (const Instance* c : i->children) if (c->cls == ClassId::Attachment) shown.insert(c->id);
    }
    Scene::forEachDescendant(const_cast<Instance&>(scene.workspace()), [&](Instance& a) {
        if (a.cls != ClassId::Attachment || (!a.visible && !shown.count(a.id))) return;
        const glm::mat4 f = a.worldFrame();
        const glm::vec3 p(f[3]);
        const bool selected = settings.selected.count(a.id) != 0;
        const glm::vec4 green(0.35f, 0.95f, 0.45f, selected ? 1.0f : 0.8f);
        const float r = 0.22f;
        const glm::vec3 d[6] = { { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r }, { 0, 0, -r } };
        // Octahedron edges.
        for (int u = 0; u < 2; ++u)
            for (int v = 2; v < 4; ++v) line(p + d[u], p + d[v], green, true);
        for (int u = 0; u < 4; ++u)
            for (int w = 4; w < 6; ++w) line(p + d[u], p + d[w], green, true);
        if (selected) {
            // Its axes: X (red, where beams curve) and Y (green, where particles leave).
            line(p, p + glm::normalize(glm::vec3(f[0])) * 0.8f, glm::vec4(0.93f, 0.33f, 0.36f, 1.0f), true);
            line(p, p + glm::normalize(glm::vec3(f[1])) * 0.8f, glm::vec4(0.45f, 0.82f, 0.36f, 1.0f), true);
        }
    });
}

// ============================================================================
//  Frame
// ============================================================================
void Renderer::render(const Scene& scene, const RenderCamera& camera, const RenderSettings& settings, Framebuffer& target) {
    stats_ = {};
    const Instance& lighting = scene.lighting();
    const SunState sun = computeSun(lighting);
    const float exposure = std::pow(2.0f, lighting.exposure);
    const glm::mat4 viewProj = camera.projection * camera.view;

    std::vector<DrawItem> opaque, transparent;
    std::vector<const Instance*> lights;
    std::vector<glm::mat4> lightFrames;
    collect(scene, camera.position, opaque, transparent, lights, lightFrames);
    stats_.parts = static_cast<int>(opaque.size() + transparent.size());
    terrain_.update(scene.terrain());
    const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_).count();

    // ---------------------------------------------------------------- shadows
    const bool shadows = settings.shadows && lighting.castShadow && sun.color.r + sun.color.g + sun.color.b > 0.01f;
    glm::mat4 lightViewProj(1.0f);
    if (shadows) {
        shadowMap_.resize(settings.shadowMapSize);
        const float radius = settings.shadowDistance * 0.5f;
        const glm::vec3 forward = -glm::vec3(glm::inverse(camera.view)[2]);
        glm::vec3 center = camera.position + forward * radius * 0.6f;
        const glm::mat4 lightView = glm::lookAt(center + sun.direction * 300.0f, center, std::abs(sun.direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
        // Snap to whole shadow-map texels so shadows do not shimmer as the camera moves.
        const float texel = 2.0f * radius / static_cast<float>(settings.shadowMapSize);
        glm::vec4 origin = lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        glm::vec2 snap(std::fmod(origin.x, texel), std::fmod(origin.y, texel));
        const glm::mat4 projection = glm::ortho(-radius - snap.x, radius - snap.x, -radius - snap.y, radius - snap.y, 1.0f, 700.0f);
        lightViewProj = projection * lightView;

        shadowMap_.bind();
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);
        glDisable(GL_CULL_FACE); // thin parts and wedges cast shadows from both sides
        shadowShader_->use();
        shadowShader_->setMat4("uLightViewProj", lightViewProj);
        for (const DrawItem& d : opaque) {
            if (!d.part->castShadow) continue;
            shadowShader_->setMat4("uModel", d.world);
            meshFor(*d.part).draw();
            ++stats_.drawCalls;
        }
        shadowShader_->setMat4("uModel", glm::mat4(1.0f)); // terrain vertices are in world space
        terrain_.drawSolid(lightViewProj);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // ---------------------------------------------------------------- main pass
    target.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);
    glEnable(GL_MULTISAMPLE);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shadows ? shadowMap_.texture() : 0);
    glActiveTexture(GL_TEXTURE0);

    // The closest lights win when there are more than the shader takes.
    std::vector<size_t> order(lights.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return glm::length(glm::vec3(lightFrames[a][3]) - camera.position) - lights[a]->range
             < glm::length(glm::vec3(lightFrames[b][3]) - camera.position) - lights[b]->range;
    });
    const int lightCount = std::min(static_cast<int>(order.size()), kMaxLights);
    stats_.lights = lightCount;
    // The lighting uniforms every lit shader shares (lighting.glsl).
    auto applyLighting = [&](Shader& s) {
        s.use();
        s.setMat4("uViewProj", viewProj);
        s.setMat4("uLightViewProj", lightViewProj);
        s.setVec3("uCameraPos", camera.position);
        s.setVec3("uSunDirection", sun.direction);
        s.setVec3("uSunColor", sun.color);
        s.setVec3("uAmbient", sun.ambient);
        s.setVec3("uSkyColor", sun.skyColor);
        s.setVec3("uHorizonColor", sun.horizonColor);
        s.setVec3("uFogColor", sun.fogColor);
        s.setFloat("uFogStart", lighting.fogStart);
        s.setFloat("uFogEnd", std::max(lighting.fogEnd, lighting.fogStart + 0.01f));
        s.setFloat("uExposure", exposure);
        s.setInt("uShadowMap", 1);
        s.setInt("uShadows", shadows ? 1 : 0);
        s.setFloat("uShadowTexel", shadows ? 1.0f / static_cast<float>(settings.shadowMapSize) : 0.0f);
        s.setFloat("uTime", time);
        s.setInt("uLightCount", lightCount);
        for (int k = 0; k < lightCount; ++k) {
            const Instance& l = *lights[order[k]];
            const glm::mat4& f = lightFrames[order[k]];
            const std::string idx = "[" + std::to_string(k) + "]";
            s.setVec4("uLightPosRange" + idx, glm::vec4(glm::vec3(f[3]), std::max(l.range, 0.01f)));
            s.setVec4("uLightColorType" + idx, glm::vec4(toLinear(l.color) * l.brightness * 1.6f, l.cls == ClassId::SpotLight ? 1.0f : 0.0f));
            s.setVec4("uLightDirCone" + idx, glm::vec4(-glm::normalize(glm::vec3(f[2])), std::cos(glm::radians(l.angle * 0.5f))));
        }
    };

    if (settings.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    // Terrain (opaque; its surfaces are closed, so back faces are skipped,
    // unless the camera is inside the ground and would see through it).
    applyLighting(*terrainShader_);
    terrainShader_->setFloat("uWave", 0.0f);
    if (scene.terrain().density(camera.position) < 0.5f) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
    stats_.triangles += terrain_.drawSolid(viewProj);
    ++stats_.drawCalls;

    applyLighting(*partShader_);
    partShader_->setVec3("uTint", settings.hoverColor);
    partShader_->setInt("uTexture", 0);
    glDisable(GL_CULL_FACE); // wedges and thin parts are visible from inside too
    drawParts(opaque, settings);

    // Sky behind everything that was drawn.
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    skyShader_->use();
    skyShader_->setMat4("uInvViewProj", glm::inverse(viewProj));
    skyShader_->setVec3("uSunDirection", sun.direction);
    skyShader_->setVec3("uSunColor", sun.day > 0.05f ? sun.color : glm::vec3(0.0f));
    skyShader_->setVec3("uSkyColor", sun.skyColor);
    skyShader_->setVec3("uHorizonColor", sun.horizonColor);
    skyShader_->setVec3("uGroundColor", sun.groundColor);
    skyShader_->setFloat("uExposure", exposure);
    skyShader_->setFloat("uDay", sun.day);
    glDepthMask(GL_FALSE);
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls;

    // Editor grid (transparent, does not write depth).
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (settings.editor && settings.grid) {
        gridShader_->use();
        gridShader_->setMat4("uViewProj", viewProj);
        gridShader_->setVec3("uCameraPos", camera.position);
        gridShader_->setFloat("uExtent", 600.0f);
        gridShader_->setFloat("uStep", std::max(settings.gridStep, 0.01f));
        gridShader_->setFloat("uFadeDistance", 350.0f);
        gridShader_->setVec3("uLineColor", glm::vec3(0.62f, 0.64f, 0.70f) * glm::mix(0.3f, 1.0f, sun.day)); // no glaring grid at night
        glBindVertexArray(gridVao_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        ++stats_.drawCalls;
    }

    // Terrain water: see-through, behind the transparent parts.
    if (terrain_.hasWater()) {
        applyLighting(*waterShader_);
        const Instance* t = scene.terrainInstance();
        waterShader_->setVec3("uWaterColor", t ? t->color.vec() : glm::vec3(0.05f, 0.33f, 0.36f));
        waterShader_->setFloat("uWaterTransparency", t ? std::clamp(t->transparency, 0.0f, 1.0f) : 0.3f);
        waterShader_->setFloat("uWave", 0.12f);
        glDepthMask(GL_FALSE);
        stats_.triangles += terrain_.drawWater(viewProj);
        ++stats_.drawCalls;
    }

    // Transparent parts, far to near.
    std::sort(transparent.begin(), transparent.end(), [](const DrawItem& a, const DrawItem& b) { return a.distance > b.distance; });
    if (!transparent.empty()) {
        partShader_->use();
        if (settings.wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        drawParts(transparent, settings);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    // Particles, beams and trails: premultiplied, so LightEmission can turn
    // normal blending (0) into adding light (1) per particle.
    {
        const glm::vec3 sceneLight = glm::clamp(sun.ambient + sun.color * std::max(sun.direction.y, 0.0f) * 0.7f, glm::vec3(0.05f), glm::vec3(1.0f));
        effects_.update(scene, settings.dt, camera.position, sceneLight);
        stats_.particles = effects_.particleCount();
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        effects_.draw(*effectShader_, textures_, viewProj);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ++stats_.drawCalls;
    }
    glDepthMask(GL_TRUE);

    // Overlays.
    if (settings.editor) addEditorOverlays(scene, settings, lights, lightFrames);
    drawLines(lines_, viewProj);
    glDisable(GL_DEPTH_TEST);
    drawLines(linesOnTop_, viewProj);
    glEnable(GL_DEPTH_TEST);
    lines_.clear();
    linesOnTop_.clear();

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    target.resolve();
}

} // namespace rift
