#include "render/Effects.h"
#include "render/Shader.h"
#include "render/TextureCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace rift {

namespace {
    constexpr size_t kMaxParticles = 4000;   // per emitter
    constexpr int kMaxBeamSegments = 256;

    float uniform(std::mt19937& rng, float lo, float hi) {
        if (hi <= lo) return lo;
        return std::uniform_real_distribution<float>(lo, hi)(rng);
    }

    // The face normal of a NormalId (Roblox: Back is +Z, Front is -Z).
    glm::vec3 faceNormal(NormalId n) {
        switch (n) {
            case NormalId::Right: return { 1, 0, 0 };
            case NormalId::Top: return { 0, 1, 0 };
            case NormalId::Back: return { 0, 0, 1 };
            case NormalId::Left: return { -1, 0, 0 };
            case NormalId::Bottom: return { 0, -1, 0 };
            case NormalId::Front: return { 0, 0, -1 };
        }
        return { 0, 1, 0 };
    }

    glm::vec3 rotateAround(const glm::vec3& v, const glm::vec3& axis, float radians) {
        const float c = std::cos(radians), s = std::sin(radians);
        return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0f - c);
    }

    // Two directions perpendicular to `n` and each other.
    void basis(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
        const glm::vec3 helper = std::abs(n.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        u = glm::normalize(glm::cross(helper, n));
        v = glm::cross(n, u);
    }

    glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
        const float l = glm::length(v);
        return l > 1e-6f ? v / l : fallback;
    }

    // Colour after brightness and how much the world's light dims it.
    glm::vec4 shade(const Color3& c, float alpha, const Instance& i, const glm::vec3& sceneLight) {
        glm::vec3 rgb = c.vec() * std::max(i.brightness, 0.0f);
        rgb *= glm::mix(glm::vec3(1.0f), glm::clamp(sceneLight, glm::vec3(0.0f), glm::vec3(1.0f)), std::clamp(i.lightInfluence, 0.0f, 1.0f));
        return { rgb, std::clamp(alpha, 0.0f, 1.0f) };
    }

    const Instance* attachment(const Scene& scene, const InstanceRef& ref) {
        if (!ref.id) return nullptr;
        const Instance* a = scene.find(ref.id);
        return a && a->cls == ClassId::Attachment && a->isDescendantOf(&scene.workspace()) ? a : nullptr;
    }
}

Effects::~Effects() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void Effects::clear() {
    scenes_.clear();
    vertices_.clear();
    batches_.clear();
}

// ============================================================================
//  Frame
// ============================================================================
void Effects::update(const Scene& scene, float dt, const glm::vec3& cameraPos, const glm::vec3& sceneLight) {
    ++frame_;
    vertices_.clear();
    batches_.clear();
    particles_ = 0;
    dt = std::clamp(dt, 0.0f, 0.1f); // a long stall does not burst out a second of particles
    SceneState& s = scenes_[&scene];
    s.frame = frame_;
    s.time += dt;

    Scene::forEachDescendant(const_cast<Instance&>(scene.workspace()), [&](Instance& i) {
        switch (i.cls) {
            case ClassId::ParticleEmitter: {
                auto [it, fresh] = s.emitters.try_emplace(i.id);
                Emitter& e = it->second;
                if (fresh) e.rng.seed(static_cast<uint32_t>(i.id ^ (i.id >> 32)));
                e.frame = frame_;
                simulate(i, e, dt);
                buildParticles(i, e, cameraPos, sceneLight);
                break;
            }
            case ClassId::Beam:
                buildBeam(scene, i, s.time, cameraPos, sceneLight);
                break;
            case ClassId::Trail: {
                Trail& t = s.trails[i.id];
                t.frame = frame_;
                buildTrail(scene, i, t, dt, cameraPos, sceneLight);
                break;
            }
            default:
                break;
        }
    });

    // Effects that are gone, and scenes that are no longer drawn (an old Play session).
    std::erase_if(s.emitters, [&](const auto& e) { return e.second.frame != frame_; });
    std::erase_if(s.trails, [&](const auto& t) { return t.second.frame != frame_; });
    std::erase_if(scenes_, [&](const auto& x) { return frame_ - x.second.frame > 240; });

    // Far to near, so see-through effects blend over the ones behind them.
    std::stable_sort(batches_.begin(), batches_.end(), [](const Batch& a, const Batch& b) { return a.depth > b.depth; });
}

void Effects::quad(const glm::vec3 c[4], const glm::vec2 uv[4], const glm::vec4 col[4], float emission, float mode) {
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k : order) vertices_.push_back({ c[k], uv[k], col[k], emission, mode });
}

// ============================================================================
//  Particles
// ============================================================================
void Effects::simulate(const Instance& em, Emitter& st, float dt) {
    if (em.clearRequests != st.clearSeen) {
        st.particles.clear();
        st.clearSeen = em.clearRequests;
    }
    const float sdt = dt * std::max(em.timeScale, 0.0f);
    const Instance* parent = em.parent;
    const bool source = parent && (parent->isBasePart() || parent->cls == ClassId::Attachment);
    const glm::mat4 frame = source ? parent->worldFrame() : glm::mat4(1.0f);
    // Locked particles live in the emitter's space; the push still comes from the world's directions.
    const glm::vec3 accel = em.lockedToPart ? glm::transpose(glm::mat3(frame)) * em.acceleration : em.acceleration;

    for (Particle& p : st.particles) {
        p.age += sdt;
        p.velocity += accel * sdt;
        if (em.drag > 0.0f) p.velocity *= std::exp(-em.drag * sdt);
        p.position += p.velocity * sdt;
        p.rotation += p.spin * sdt;
    }
    std::erase_if(st.particles, [](const Particle& p) { return p.age >= p.life; });

    int spawn = 0;
    if (em.enabled && em.rate > 0.0f && source) {
        st.pending += em.rate * sdt;
        spawn = static_cast<int>(st.pending);
        st.pending -= static_cast<float>(spawn);
    } else {
        st.pending = 0.0f;
    }
    if (em.emitRequests > st.emitSeen) spawn += em.emitRequests - st.emitSeen;
    st.emitSeen = em.emitRequests;
    if (!source) return;
    spawn = std::min(spawn, static_cast<int>(kMaxParticles - std::min(kMaxParticles, st.particles.size())));
    if (spawn <= 0) return;

    const glm::vec3 n = faceNormal(em.emissionDirection);
    glm::vec3 u, v;
    basis(n, u, v);
    const glm::vec3 half = parent->isBasePart() ? parent->size * 0.5f : glm::vec3(0.0f);
    const glm::mat3 rot(frame);
    const glm::vec3 origin(frame[3]);
    for (int k = 0; k < spawn; ++k) {
        Particle p;
        // Somewhere on the emitting face of the part (or the attachment's point).
        glm::vec3 local = n * glm::abs(glm::dot(n, half));
        local += u * uniform(st.rng, -1.0f, 1.0f) * glm::abs(glm::dot(u, half));
        local += v * uniform(st.rng, -1.0f, 1.0f) * glm::abs(glm::dot(v, half));
        // Leaving within SpreadAngle of the face's direction.
        glm::vec3 dir = n;
        dir = rotateAround(dir, u, glm::radians(uniform(st.rng, -em.spreadAngle.x, em.spreadAngle.x)));
        dir = rotateAround(dir, v, glm::radians(uniform(st.rng, -em.spreadAngle.y, em.spreadAngle.y)));
        const float speed = uniform(st.rng, em.speedRange.min, em.speedRange.max);
        if (em.lockedToPart) {
            p.position = local;
            p.velocity = dir * speed;
        } else {
            p.position = origin + rot * local;
            p.velocity = rot * dir * speed;
        }
        p.life = std::max(uniform(st.rng, em.lifetime.min, em.lifetime.max), 0.01f);
        p.rotation = uniform(st.rng, em.rotationRange.min, em.rotationRange.max);
        p.spin = uniform(st.rng, em.rotSpeed.min, em.rotSpeed.max);
        p.sizeNoise = uniform(st.rng, -1.0f, 1.0f);
        p.fadeNoise = uniform(st.rng, -1.0f, 1.0f);
        st.particles.push_back(p);
    }
}

void Effects::buildParticles(const Instance& em, const Emitter& st, const glm::vec3& cameraPos, const glm::vec3& sceneLight) {
    if (st.particles.empty()) return;
    const Instance* parent = em.parent;
    const glm::mat4 frame = parent && (parent->isBasePart() || parent->cls == ClassId::Attachment) ? parent->worldFrame() : glm::mat4(1.0f);
    const glm::mat3 rot(frame);

    struct Drawn { glm::vec3 position, velocity; float distance; const Particle* p; };
    std::vector<Drawn> drawn;
    drawn.reserve(st.particles.size());
    glm::vec3 centre(0.0f);
    for (const Particle& p : st.particles) {
        Drawn d;
        d.position = em.lockedToPart ? glm::vec3(frame * glm::vec4(p.position, 1.0f)) : p.position;
        d.velocity = em.lockedToPart ? rot * p.velocity : p.velocity;
        d.distance = glm::length(d.position - cameraPos);
        d.p = &p;
        drawn.push_back(d);
        centre += d.position;
    }
    centre /= static_cast<float>(drawn.size());
    std::sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.distance > b.distance; });

    Batch batch;
    batch.texture = em.texture.path;
    batch.first = vertices_.size();
    batch.depth = glm::length(centre - cameraPos);
    const float mode = em.texture.path.empty() ? 1.0f : 0.0f;
    const float emission = std::clamp(em.lightEmission, 0.0f, 1.0f);
    static const glm::vec2 uvs[4] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
    for (const Drawn& d : drawn) {
        const Particle& p = *d.p;
        const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        const float size = em.sizeSequence.at(t) + em.sizeSequence.envelopeAt(t) * p.sizeNoise;
        const float transparency = em.transparencySequence.at(t) + em.transparencySequence.envelopeAt(t) * p.fadeNoise;
        if (size <= 0.0f || transparency >= 0.999f) continue;
        const glm::vec3 toCam = safeNormalize(cameraPos - d.position, glm::vec3(0, 0, 1));
        glm::vec3 right, up;
        float spin = glm::radians(p.rotation);
        switch (em.particleOrientation) {
            case ParticleOrientation::FacingCamera:
                right = safeNormalize(glm::cross(glm::vec3(0, 1, 0), toCam), glm::vec3(1, 0, 0));
                up = glm::cross(toCam, right);
                break;
            case ParticleOrientation::FacingCameraWorldUp:
                up = glm::vec3(0, 1, 0);
                right = safeNormalize(glm::cross(up, toCam), glm::vec3(1, 0, 0));
                spin = 0.0f;
                break;
            case ParticleOrientation::VelocityParallel:
                up = safeNormalize(d.velocity, glm::vec3(0, 1, 0));
                right = safeNormalize(glm::cross(up, toCam), glm::vec3(1, 0, 0));
                spin = 0.0f;
                break;
            case ParticleOrientation::VelocityPerpendicular:
                basis(safeNormalize(d.velocity, glm::vec3(0, 1, 0)), right, up);
                break;
        }
        if (spin != 0.0f) {
            const glm::vec3 axis = glm::normalize(glm::cross(right, up));
            right = rotateAround(right, axis, spin);
            up = rotateAround(up, axis, spin);
        }
        const float h = size * 0.5f;
        const glm::vec3 c = d.position + toCam * em.zOffset;
        const glm::vec3 corners[4] = { c - right * h - up * h, c + right * h - up * h, c + right * h + up * h, c - right * h + up * h };
        const glm::vec4 col = shade(em.colorSequence.at(t), 1.0f - std::clamp(transparency, 0.0f, 1.0f), em, sceneLight);
        const glm::vec4 cols[4] = { col, col, col, col };
        quad(corners, uvs, cols, emission, mode);
        ++particles_;
    }
    batch.count = vertices_.size() - batch.first;
    if (batch.count) batches_.push_back(batch);
}

// ============================================================================
//  Beams
// ============================================================================
void Effects::buildBeam(const Scene& scene, const Instance& beam, float time, const glm::vec3& cameraPos, const glm::vec3& sceneLight) {
    if (!beam.enabled) return;
    const Instance* a0 = attachment(scene, beam.attachment0);
    const Instance* a1 = attachment(scene, beam.attachment1);
    if (!a0 || !a1) return;
    const glm::mat4 f0 = a0->worldFrame(), f1 = a1->worldFrame();
    const glm::vec3 p0(f0[3]), p1(f1[3]);
    const glm::vec3 x0 = safeNormalize(glm::vec3(f0[0]), { 1, 0, 0 }), x1 = safeNormalize(glm::vec3(f1[0]), { 1, 0, 0 });
    const glm::vec3 y0 = safeNormalize(glm::vec3(f0[1]), { 0, 1, 0 }), y1 = safeNormalize(glm::vec3(f1[1]), { 0, 1, 0 });
    // A cubic Bezier: the middle points bend out along each attachment's X axis.
    const glm::vec3 c0 = p0 + x0 * beam.curveSize0, c1 = p1 - x1 * beam.curveSize1;
    auto point = [&](float t) {
        const float s = 1.0f - t;
        return s * s * s * p0 + 3.0f * s * s * t * c0 + 3.0f * s * t * t * c1 + t * t * t * p1;
    };
    auto tangent = [&](float t) {
        const float s = 1.0f - t;
        return safeNormalize(3.0f * s * s * (c0 - p0) + 6.0f * s * t * (c1 - c0) + 3.0f * t * t * (p1 - c1), safeNormalize(p1 - p0, { 0, 0, 1 }));
    };
    const int n = std::clamp(beam.segments, 1, kMaxBeamSegments);
    std::vector<glm::vec3> pts(static_cast<size_t>(n) + 1);
    std::vector<float> along(static_cast<size_t>(n) + 1, 0.0f);
    for (int k = 0; k <= n; ++k) {
        pts[static_cast<size_t>(k)] = point(static_cast<float>(k) / n);
        if (k > 0) along[static_cast<size_t>(k)] = along[static_cast<size_t>(k) - 1] + glm::length(pts[static_cast<size_t>(k)] - pts[static_cast<size_t>(k) - 1]);
    }
    const float length = std::max(along.back(), 1e-4f);
    const float repeat = std::max(beam.textureLength, 0.01f);
    const float scroll = time * beam.textureSpeed;

    Batch batch;
    batch.texture = beam.texture.path;
    batch.first = vertices_.size();
    batch.depth = glm::length((p0 + p1) * 0.5f - cameraPos);
    const float mode = beam.texture.path.empty() ? 2.0f : 0.0f;
    const float emission = std::clamp(beam.lightEmission, 0.0f, 1.0f);
    auto edge = [&](int k, glm::vec3& left, glm::vec3& right, glm::vec4& col, float& u) {
        const float t = static_cast<float>(k) / n;
        const glm::vec3 p = pts[static_cast<size_t>(k)];
        const glm::vec3 dir = tangent(t);
        glm::vec3 side;
        if (beam.faceCamera) {
            side = safeNormalize(glm::cross(dir, cameraPos - p), glm::vec3(1, 0, 0));
        } else {
            const glm::vec3 up = safeNormalize(glm::mix(y0, y1, t), glm::vec3(0, 1, 0));
            side = safeNormalize(glm::cross(dir, up), safeNormalize(glm::cross(dir, cameraPos - p), glm::vec3(1, 0, 0)));
        }
        const float w = glm::mix(beam.width0, beam.width1, t) * 0.5f;
        left = p - side * w;
        right = p + side * w;
        col = shade(beam.colorSequence.at(t), 1.0f - std::clamp(beam.transparencySequence.at(t), 0.0f, 1.0f), beam, sceneLight);
        const float d = along[static_cast<size_t>(k)];
        u = (beam.textureMode == TextureMode::Stretch ? d / length * repeat : d / repeat) - scroll;
    };
    for (int k = 0; k < n; ++k) {
        glm::vec3 l0, r0, l1, r1;
        glm::vec4 col0, col1;
        float u0, u1;
        edge(k, l0, r0, col0, u0);
        edge(k + 1, l1, r1, col1, u1);
        if (col0.a <= 0.001f && col1.a <= 0.001f) continue;
        const glm::vec3 corners[4] = { l0, r0, r1, l1 };
        const glm::vec2 uvs[4] = { { u0, 0.0f }, { u0, 1.0f }, { u1, 1.0f }, { u1, 0.0f } };
        const glm::vec4 cols[4] = { col0, col0, col1, col1 };
        quad(corners, uvs, cols, emission, mode);
    }
    batch.count = vertices_.size() - batch.first;
    if (batch.count) batches_.push_back(batch);
}

// ============================================================================
//  Trails
// ============================================================================
void Effects::buildTrail(const Scene& scene, const Instance& trail, Trail& st, float dt, const glm::vec3& cameraPos, const glm::vec3& sceneLight) {
    const Instance* a0 = attachment(scene, trail.attachment0);
    const Instance* a1 = attachment(scene, trail.attachment1);
    if (!a0 || !a1) {
        st.points.clear();
        return;
    }
    st.time += dt;
    const float lifetime = std::max(trail.trailLifetime, 0.01f);
    const glm::vec3 a = a0->worldPosition(), b = a1->worldPosition();
    // A new point once the attachments moved MinLength from the last one.
    if (trail.enabled && dt > 0.0f) {
        const glm::vec3 mid = (a + b) * 0.5f;
        if (!st.started || glm::length(mid - st.lastMid) >= std::max(trail.minLength, 0.001f)) {
            st.points.push_back({ a, b, st.time });
            st.lastMid = mid;
            st.started = true;
        }
    }
    while (!st.points.empty() && st.time - st.points.front().time > lifetime) st.points.pop_front();
    if (st.points.size() > 2048) st.points.erase(st.points.begin(), st.points.begin() + static_cast<std::ptrdiff_t>(st.points.size() - 2048));

    // Newest first: the attachments themselves, then the points behind them.
    std::vector<TrailPoint> chain;
    if (trail.enabled) chain.push_back({ a, b, st.time });
    for (auto it = st.points.rbegin(); it != st.points.rend(); ++it) chain.push_back(*it);
    if (chain.size() < 2) return;
    if (trail.maxLength > 0.0f) {
        float total = 0.0f;
        for (size_t k = 1; k < chain.size(); ++k) {
            total += glm::length((chain[k].a + chain[k].b) - (chain[k - 1].a + chain[k - 1].b)) * 0.5f;
            if (total > trail.maxLength) { chain.resize(k + 1); break; }
        }
    }

    Batch batch;
    batch.texture = trail.texture.path;
    batch.first = vertices_.size();
    batch.depth = glm::length((a + b) * 0.5f - cameraPos);
    const float mode = trail.texture.path.empty() ? 2.0f : 0.0f;
    const float emission = std::clamp(trail.lightEmission, 0.0f, 1.0f);
    const float repeat = std::max(trail.textureLength, 0.01f);
    struct Edge { glm::vec3 l, r; glm::vec4 col; float u; };
    std::vector<Edge> edges(chain.size());
    float along = 0.0f;
    for (size_t k = 0; k < chain.size(); ++k) {
        const TrailPoint& p = chain[k];
        const float t = std::clamp((st.time - p.time) / lifetime, 0.0f, 1.0f);
        const glm::vec3 mid = (p.a + p.b) * 0.5f;
        if (k > 0) along += glm::length(mid - (chain[k - 1].a + chain[k - 1].b) * 0.5f);
        const float scale = std::max(trail.sizeSequence.at(t), 0.0f);
        glm::vec3 half = (p.b - p.a) * 0.5f;
        if (trail.faceCamera) {
            const glm::vec3 next = k + 1 < chain.size() ? (chain[k + 1].a + chain[k + 1].b) * 0.5f : mid;
            const glm::vec3 prev = k > 0 ? (chain[k - 1].a + chain[k - 1].b) * 0.5f : mid;
            const glm::vec3 dir = safeNormalize(next - prev, glm::vec3(0, 1, 0));
            half = safeNormalize(glm::cross(dir, cameraPos - mid), glm::vec3(1, 0, 0)) * glm::length(half);
        }
        Edge& e = edges[k];
        e.l = mid - half * scale;
        e.r = mid + half * scale;
        e.col = shade(trail.colorSequence.at(t), 1.0f - std::clamp(trail.transparencySequence.at(t), 0.0f, 1.0f), trail, sceneLight);
        e.u = trail.textureMode == TextureMode::Stretch ? t * repeat : along / repeat;
    }
    for (size_t k = 0; k + 1 < edges.size(); ++k) {
        const Edge& e0 = edges[k];
        const Edge& e1 = edges[k + 1];
        if (e0.col.a <= 0.001f && e1.col.a <= 0.001f) continue;
        if (glm::length((e0.l + e0.r) - (e1.l + e1.r)) < 1e-4f) continue; // no length: nothing to see
        const glm::vec3 corners[4] = { e0.l, e0.r, e1.r, e1.l };
        const glm::vec2 uvs[4] = { { e0.u, 0.0f }, { e0.u, 1.0f }, { e1.u, 1.0f }, { e1.u, 0.0f } };
        const glm::vec4 cols[4] = { e0.col, e0.col, e1.col, e1.col };
        quad(corners, uvs, cols, emission, mode);
    }
    batch.count = vertices_.size() - batch.first;
    if (batch.count) batches_.push_back(batch);
}

// ============================================================================
//  Drawing
// ============================================================================
void Effects::draw(Shader& shader, TextureCache& textures, const glm::mat4& viewProj) {
    if (vertices_.empty() || batches_.empty()) return;
    if (!vao_) {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, emission)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, mode)));
    }
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    if (vertices_.size() > capacity_) {
        capacity_ = vertices_.size() * 3 / 2 + 1024;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_ * sizeof(Vertex)), nullptr, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)), vertices_.data());

    shader.use();
    shader.setMat4("uViewProj", viewProj);
    shader.setInt("uTexture", 0);
    for (const Batch& b : batches_) {
        std::shared_ptr<Texture> tex = b.texture.empty() ? nullptr : textures.get(b.texture);
        shader.setInt("uTextured", tex ? 1 : 0);
        if (tex) tex->bind(0);
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(b.first), static_cast<GLsizei>(b.count));
    }
    glBindVertexArray(0);
}

} // namespace rift
