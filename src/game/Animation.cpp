#include "game/Animation.h"
#include "core/Paths.h"
#include "core/TransformUtil.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>

namespace rift {

namespace {
    constexpr float kPi = 3.14159265358979f;

    glm::quat eulerQuat(const glm::vec3& deg) { return glm::quat_cast(glm::mat3(TransformUtil::compose(glm::vec3(0.0f), deg, glm::vec3(1.0f)))); }
    glm::vec3 quatEuler(const glm::quat& q) {
        glm::vec3 p, deg, s;
        TransformUtil::decompose(glm::mat4(glm::mat3_cast(q)), p, deg, s);
        return deg;
    }

    template <class E> E enumByName(const std::vector<std::string>& names, const std::string& name, E fallback) {
        auto it = std::find(names.begin(), names.end(), name);
        return it == names.end() ? fallback : static_cast<E>(it - names.begin());
    }
    const std::vector<std::string>& priorityNames() {
        static const std::vector<std::string> n = { "Idle", "Movement", "Action", "Action2", "Action3", "Action4", "Core" };
        return n;
    }
    const std::vector<std::string>& styleNames() {
        static const std::vector<std::string> n = { "Linear", "Constant", "Elastic", "Cubic", "Bounce", "CubicV2" };
        return n;
    }
    const std::vector<std::string>& directionNames() {
        static const std::vector<std::string> n = { "In", "Out", "InOut" };
        return n;
    }

    float bounceOut(float t) {
        const float n = 7.5625f, d = 2.75f;
        if (t < 1.0f / d) return n * t * t;
        if (t < 2.0f / d) { t -= 1.5f / d; return n * t * t + 0.75f; }
        if (t < 2.5f / d) { t -= 2.25f / d; return n * t * t + 0.9375f; }
        t -= 2.625f / d;
        return n * t * t + 0.984375f;
    }
}

// ============================================================================
//  Clips
// ============================================================================
void AnimClip::prepare() {
    std::stable_sort(keyframes.begin(), keyframes.end(), [](const AnimKeyframe& a, const AnimKeyframe& b) { return a.time < b.time; });
    std::stable_sort(markers.begin(), markers.end(), [](const AnimMarker& a, const AnimMarker& b) { return a.time < b.time; });
    channels.clear();
    for (const AnimKeyframe& k : keyframes)
        for (const auto& [joint, pose] : k.poses) channels[joint].emplace_back(k.time, pose);
}

float easePose(PoseEasingStyle style, PoseEasingDirection dir, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto in = [&](float x) -> float {
        switch (style) {
            case PoseEasingStyle::Linear: return x;
            case PoseEasingStyle::Constant: return 0.0f;
            case PoseEasingStyle::Cubic:
            case PoseEasingStyle::CubicV2: return x * x * x;
            case PoseEasingStyle::Elastic: {
                if (x <= 0.0f || x >= 1.0f) return x;
                return -std::pow(2.0f, 10.0f * x - 10.0f) * std::sin((x * 10.0f - 10.75f) * (2.0f * kPi / 3.0f));
            }
            case PoseEasingStyle::Bounce: return 1.0f - bounceOut(1.0f - x);
        }
        return x;
    };
    if (style == PoseEasingStyle::Linear || style == PoseEasingStyle::Constant) return in(t);
    switch (dir) {
        case PoseEasingDirection::In: return in(t);
        case PoseEasingDirection::Out: return 1.0f - in(1.0f - t);
        case PoseEasingDirection::InOut: return t < 0.5f ? in(t * 2.0f) * 0.5f : 1.0f - in((1.0f - t) * 2.0f) * 0.5f;
    }
    return t;
}

int priorityRank(AnimationPriority p) {
    return p == AnimationPriority::Core ? 0 : static_cast<int>(p) + 1;
}

void sampleClip(const AnimClip& clip, float time, std::unordered_map<std::string, AnimPose>& out) {
    out.clear();
    for (const auto& [joint, keys] : clip.channels) {
        if (keys.empty()) continue;
        if (time <= keys.front().first) { out[joint] = keys.front().second; continue; }
        if (time >= keys.back().first) { out[joint] = keys.back().second; continue; }
        auto next = std::upper_bound(keys.begin(), keys.end(), time, [](float t, const std::pair<float, AnimPose>& k) { return t < k.first; });
        const auto& b = *next;
        const auto& a = *(next - 1);
        const float span = b.first - a.first;
        const float u = span > 1e-6f ? (time - a.first) / span : 1.0f;
        const float e = easePose(a.second.style, a.second.direction, u);
        AnimPose p = a.second;
        p.position = glm::mix(a.second.position, b.second.position, e);
        p.rotation = glm::slerp(a.second.rotation, b.second.rotation, e);
        p.weight = a.second.weight + (b.second.weight - a.second.weight) * e;
        out[joint] = p;
    }
}

std::shared_ptr<AnimClip> clipFromJson(const nlohmann::json& j, std::string* error) {
    auto fail = [&](const std::string& why) -> std::shared_ptr<AnimClip> {
        if (error) *error = why;
        return nullptr;
    };
    if (!j.is_object() || !j.contains("keyframes") || !j["keyframes"].is_array()) return fail("not an animation (no keyframes)");
    auto clip = std::make_shared<AnimClip>();
    clip->name = j.value("name", std::string());
    clip->loop = j.value("loop", false);
    clip->priority = enumByName(priorityNames(), j.value("priority", std::string("Core")), AnimationPriority::Core);
    auto readFrame = [](const nlohmann::json& a, AnimPose& p) {
        if (!a.is_array() || a.size() != 6) return false;
        for (const auto& x : a) if (!x.is_number()) return false;
        p.position = { a[0].get<float>(), a[1].get<float>(), a[2].get<float>() };
        p.rotation = eulerQuat({ a[3].get<float>(), a[4].get<float>(), a[5].get<float>() });
        return true;
    };
    for (const auto& k : j["keyframes"]) {
        if (!k.is_object()) return fail("a keyframe is not an object");
        AnimKeyframe key;
        key.time = std::max(k.value("time", 0.0f), 0.0f);
        key.name = k.value("name", std::string());
        if (k.contains("poses") && k["poses"].is_object()) {
            for (auto it = k["poses"].begin(); it != k["poses"].end(); ++it) {
                AnimPose p;
                const auto& v = it.value();
                if (v.is_object()) {
                    if (!readFrame(v.value("cf", nlohmann::json()), p)) return fail("pose " + it.key() + " has no cf");
                    p.weight = v.value("weight", 1.0f);
                    p.style = enumByName(styleNames(), v.value("style", std::string("Linear")), PoseEasingStyle::Linear);
                    p.direction = enumByName(directionNames(), v.value("direction", std::string("In")), PoseEasingDirection::In);
                } else if (!readFrame(v, p)) {
                    return fail("pose " + it.key() + " is not [x, y, z, rx, ry, rz]");
                }
                key.poses[it.key()] = p;
            }
        }
        clip->keyframes.push_back(std::move(key));
    }
    if (j.contains("markers") && j["markers"].is_array()) {
        for (const auto& m : j["markers"]) {
            if (!m.is_object()) continue;
            clip->markers.push_back({ m.value("time", 0.0f), m.value("name", std::string()), m.value("value", std::string()) });
        }
    }
    clip->prepare();
    return clip;
}

nlohmann::json clipToJson(const AnimClip& clip) {
    using J = nlohmann::json;
    auto round4 = [](float v) { const double r = std::round(static_cast<double>(v) * 1e4) / 1e4; return r == 0.0 ? 0.0 : r; };
    J out = J::object();
    out["format"] = "rift-animation";
    out["version"] = 1;
    out["name"] = clip.name;
    out["loop"] = clip.loop;
    out["priority"] = priorityNames()[static_cast<size_t>(clip.priority)];
    J keys = J::array();
    for (const AnimKeyframe& k : clip.keyframes) {
        J key = J::object();
        key["time"] = round4(k.time);
        if (!k.name.empty()) key["name"] = k.name;
        J poses = J::object();
        // Sorted joint names: stable files for version control.
        std::vector<std::string> names;
        for (const auto& [name, pose] : k.poses) names.push_back(name);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            const AnimPose& p = k.poses.at(name);
            const glm::vec3 r = quatEuler(p.rotation);
            J cf = J::array({ round4(p.position.x), round4(p.position.y), round4(p.position.z), round4(r.x), round4(r.y), round4(r.z) });
            if (p.style == PoseEasingStyle::Linear && p.direction == PoseEasingDirection::In && p.weight == 1.0f) {
                poses[name] = cf;
            } else {
                J o = J::object();
                o["cf"] = cf;
                if (p.weight != 1.0f) o["weight"] = round4(p.weight);
                o["style"] = styleNames()[static_cast<size_t>(p.style)];
                o["direction"] = directionNames()[static_cast<size_t>(p.direction)];
                poses[name] = o;
            }
        }
        key["poses"] = poses;
        keys.push_back(key);
    }
    out["keyframes"] = keys;
    if (!clip.markers.empty()) {
        J markers = J::array();
        for (const AnimMarker& m : clip.markers) markers.push_back({ { "time", round4(m.time) }, { "name", m.name }, { "value", m.value } });
        out["markers"] = markers;
    }
    return out;
}

std::shared_ptr<AnimClip> clipFromSequence(const Instance& kfs) {
    auto clip = std::make_shared<AnimClip>();
    clip->name = kfs.name;
    clip->loop = kfs.looped;
    clip->priority = kfs.priority;
    for (const Instance* k : kfs.children) {
        if (k->cls != ClassId::Keyframe) continue;
        AnimKeyframe key;
        key.time = std::max(k->keyframeTime, 0.0f);
        key.name = k->name == "Keyframe" ? std::string() : k->name; // Roblox: unnamed keyframes do not fire KeyframeReached
        Scene::forEachDescendant(const_cast<Instance&>(*k), [&](Instance& p) {
            if (p.cls == ClassId::Pose) {
                AnimPose pose = poseFromMatrix(p.c0.matrix());
                pose.weight = p.poseWeight;
                pose.style = p.easingStyle;
                pose.direction = p.easingDirection;
                key.poses[p.name] = pose;
            } else if (p.cls == ClassId::KeyframeMarker) {
                clip->markers.push_back({ key.time, p.name, p.text });
            }
        });
        clip->keyframes.push_back(std::move(key));
    }
    clip->prepare();
    return clip;
}

void clipToSequence(const AnimClip& clip, Scene& scene, Instance& kfs) {
    kfs.looped = clip.loop;
    kfs.priority = clip.priority;
    // Markers live in Keyframes: one at a time no keyframe has gets an empty keyframe.
    std::vector<AnimKeyframe> keyframes = clip.keyframes;
    for (const AnimMarker& m : clip.markers) {
        const bool found = std::any_of(keyframes.begin(), keyframes.end(), [&](const AnimKeyframe& k) { return std::abs(k.time - m.time) <= 1e-4f; });
        if (!found) {
            AnimKeyframe holder;
            holder.time = m.time;
            keyframes.push_back(holder);
        }
    }
    for (const AnimKeyframe& k : keyframes) {
        Instance* key = scene.create(ClassId::Keyframe, &kfs);
        key->name = k.name.empty() ? "Keyframe" : k.name;
        key->keyframeTime = k.time;
        std::vector<std::string> names;
        for (const auto& [name, pose] : k.poses) names.push_back(name);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            const AnimPose& p = k.poses.at(name);
            Instance* pose = scene.create(ClassId::Pose, key);
            pose->name = name;
            pose->c0 = CFrameValue::fromMatrix(poseMatrix(p));
            pose->poseWeight = p.weight;
            pose->easingStyle = p.style;
            pose->easingDirection = p.direction;
        }
        for (const AnimMarker& m : clip.markers) {
            if (std::abs(m.time - k.time) > 1e-4f) continue;
            Instance* marker = scene.create(ClassId::KeyframeMarker, key);
            marker->name = m.name;
            marker->text = m.value;
        }
    }
}

bool saveClip(const AnimClip& clip, const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "cannot write " + Paths::toUtf8(file);
        return false;
    }
    out << clipToJson(clip).dump(2) << "\n";
    return !out.fail();
}

std::shared_ptr<AnimClip> loadClipFile(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        if (error) *error = "file not found: " + Paths::toUtf8(file);
        return nullptr;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = std::string("not valid JSON: ") + e.what();
        return nullptr;
    }
    auto clip = clipFromJson(j, error);
    if (clip && clip->name.empty()) clip->name = Paths::toUtf8(file.stem());
    return clip;
}

// ============================================================================
//  Joints
// ============================================================================
glm::mat4 poseMatrix(const AnimPose& pose) {
    glm::mat4 m = glm::mat4_cast(pose.rotation);
    m[3] = glm::vec4(pose.position, 1.0f);
    return m;
}

AnimPose poseFromMatrix(const glm::mat4& m) {
    AnimPose p;
    p.position = glm::vec3(m[3]);
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float l = glm::length(r[i]);
        if (l > 1e-8f) r[i] /= l;
    }
    p.rotation = glm::normalize(glm::quat_cast(r));
    return p;
}

std::unordered_map<std::string, Instance*> rigJoints(Instance& rig) {
    std::unordered_map<std::string, Instance*> out;
    Scene* scene = rig.scene;
    Scene::forEachDescendant(rig, [&](Instance& m) {
        if (m.cls != ClassId::Motor6D || !scene) return;
        const Instance* part1 = scene->find(m.attachment1.id);
        if (part1) out.emplace(part1->name, &m);
    });
    return out;
}

Instance* rigOf(const Instance& animator) {
    Instance* p = animator.parent;
    if (p && (p->cls == ClassId::Humanoid || p->cls == ClassId::AnimationController)) p = p->parent;
    return p;
}

void solveJoints(Scene& scene, Instance& root) {
    // Part1 -> its joint.
    std::unordered_map<uint64_t, Instance*> byPart1;
    Scene::forEachDescendant(root, [&](Instance& m) {
        if (m.cls != ClassId::Motor6D || !m.enabled || !m.attachment0.id || !m.attachment1.id || m.attachment0.id == m.attachment1.id) return;
        byPart1.emplace(m.attachment1.id, &m);
    });
    if (byPart1.empty()) return;
    std::unordered_set<uint64_t> done, visiting;
    std::function<void(uint64_t)> solve = [&](uint64_t partId) {
        if (done.count(partId) || visiting.count(partId)) return;
        auto it = byPart1.find(partId);
        if (it == byPart1.end()) { done.insert(partId); return; }
        visiting.insert(partId);
        const Instance& motor = *it->second;
        solve(motor.attachment0.id); // its parent joint first
        Instance* part0 = scene.find(motor.attachment0.id);
        Instance* part1 = scene.find(partId);
        if (part0 && part1 && part0->isBasePart() && part1->isBasePart()) {
            const glm::mat4 world = part0->worldFrame() * motor.c0.matrix() * motor.jointTransform.matrix() * glm::inverse(motor.c1.matrix());
            glm::vec3 pos, deg, scale;
            TransformUtil::decompose(world, pos, deg, scale);
            part1->setWorldFrame(pos, deg);
        }
        visiting.erase(partId);
        done.insert(partId);
    };
    for (const auto& [part1, motor] : byPart1) solve(part1);
}

Instance* buildCharacterRig(Scene& scene, const RigColors& colors, bool anchored) {
    Instance* model = scene.create(ClassId::Model);
    model->name = "Rig";
    auto part = [&](Instance* parent, const char* name, PartShape shape, glm::vec3 pos, glm::vec3 size, Color3 color) {
        Instance* p = scene.create(ClassId::Part, parent);
        p->name = name;
        p->shape = shape;
        p->position = pos;
        p->size = size;
        p->color = color;
        p->material = Material::SmoothPlastic;
        p->anchored = anchored;
        p->canCollide = false;
        return p;
    };
    Instance* root = part(model, "HumanoidRootPart", PartShape::Block, { 0, 0, 0 }, { 1.0f, 2.0f, 1.0f }, colors.shirt);
    root->transparency = 1.0f;
    root->castShadow = false;
    Instance* torso = part(model, "Torso", PartShape::Block, { 0, 0.15f, 0 }, { 1.0f, 0.8f, 0.55f }, colors.shirt);
    Instance* head = part(model, "Head", PartShape::Ball, { 0, 0.8f, 0 }, { 0.55f, 0.55f, 0.55f }, colors.skin);
    Instance* leftArm = part(model, "Left Arm", PartShape::Block, { -0.66f, 0.12f, 0 }, { 0.3f, 0.75f, 0.32f }, colors.skin);
    Instance* rightArm = part(model, "Right Arm", PartShape::Block, { 0.66f, 0.12f, 0 }, { 0.3f, 0.75f, 0.32f }, colors.skin);
    Instance* leftLeg = part(model, "Left Leg", PartShape::Block, { -0.24f, -0.625f, 0 }, { 0.42f, 0.75f, 0.45f }, colors.pants);
    Instance* rightLeg = part(model, "Right Leg", PartShape::Block, { 0.24f, -0.625f, 0 }, { 0.42f, 0.75f, 0.45f }, colors.pants);
    // The eyes ride on the head.
    for (float side : { -1.0f, 1.0f }) {
        Instance* eye = part(head, side < 0.0f ? "LeftEye" : "RightEye", PartShape::Ball, { 0.1f * side, 0.06f, -0.24f }, { 0.08f, 0.1f, 0.06f }, { 0.08f, 0.08f, 0.1f });
        eye->castShadow = false;
    }
    // Joints: where each limb turns (C0 in the parent part, C1 in the limb).
    auto joint = [&](Instance* in, const char* name, Instance* p0, Instance* p1, glm::vec3 c0, glm::vec3 c1) {
        Instance* m = scene.create(ClassId::Motor6D, in);
        m->name = name;
        m->attachment0.id = p0->id;
        m->attachment1.id = p1->id;
        m->c0.position = c0;
        m->c1.position = c1;
    };
    joint(root, "RootJoint", root, torso, { 0, 0.15f, 0 }, { 0, 0, 0 });
    joint(torso, "Neck", torso, head, { 0, 0.4f, 0 }, { 0, -0.25f, 0 });
    joint(torso, "Left Shoulder", torso, leftArm, { -0.66f, 0.345f, 0 }, { 0, 0.375f, 0 });
    joint(torso, "Right Shoulder", torso, rightArm, { 0.66f, 0.345f, 0 }, { 0, 0.375f, 0 });
    joint(torso, "Left Hip", torso, leftLeg, { -0.24f, -0.4f, 0 }, { 0, 0.375f, 0 });
    joint(torso, "Right Hip", torso, rightLeg, { 0.24f, -0.4f, 0 }, { 0, 0.375f, 0 });
    Instance* hum = scene.create(ClassId::Humanoid, model);
    scene.create(ClassId::Animator, hum);
    return model;
}

// ============================================================================
//  Tracks
// ============================================================================
std::shared_ptr<AnimClip> AnimationSystem::clipFor(const std::string& animationId, Scene& from, std::string& error) {
    if (animationId.empty()) {
        error = "the Animation has no AnimationId";
        return nullptr;
    }
    // A KeyframeSequence in the world (KeyframeSequenceProvider, the Animation Editor).
    if (animationId.rfind("kfs:", 0) == 0) {
        const uint64_t id = idFromString(animationId.substr(4));
        const Instance* kfs = id ? from.find(id) : nullptr;
        if (!kfs && server_ && server_ != &from) kfs = server_->find(id);
        if (!kfs || kfs->cls != ClassId::KeyframeSequence) {
            error = "no KeyframeSequence for " + animationId;
            return nullptr;
        }
        return clipFromSequence(*kfs);
    }
    if (animationId.rfind("rbxassetid://", 0) == 0 || animationId.rfind("http", 0) == 0) {
        error = "Roblox asset ids cannot be loaded: make the animation in the Animation Editor and use its .anim file";
        return nullptr;
    }
    const std::filesystem::path file = Paths::resolveAsset(animationId);
    const std::string key = Paths::toUtf8(file);
    if (auto it = files_.find(key); it != files_.end()) return it->second;
    auto clip = loadClipFile(file, &error);
    if (!clip) {
        error = "cannot load animation " + animationId + ": " + error;
        return nullptr;
    }
    files_[key] = clip;
    return clip;
}

uint64_t AnimationSystem::load(int owner, Scene& from, const Instance& animator, const Instance& animation, std::string& error) {
    auto clip = clipFor(animation.animationId, from, error);
    if (!clip) return 0;
    Track t;
    t.id = nextId_++;
    t.owner = owner;
    // A rig the server has (a player's character) animates in the server's world.
    t.scene = server_ && server_->find(animator.id) ? server_ : &from;
    t.animator = animator.id;
    t.animation = animation.id;
    t.name = animation.name;
    t.clip = clip;
    t.looped = clip->loop;
    t.priority = clip->priority;
    tracks_[t.id] = t;
    order_.push_back(t.id);
    return t.id;
}

AnimationSystem::Track* AnimationSystem::find(uint64_t id) {
    auto it = tracks_.find(id);
    return it == tracks_.end() ? nullptr : &it->second;
}

const AnimationSystem::Track* AnimationSystem::find(uint64_t id) const {
    auto it = tracks_.find(id);
    return it == tracks_.end() ? nullptr : &it->second;
}

TrackInfo AnimationSystem::info(uint64_t id) const {
    TrackInfo i;
    const Track* t = find(id);
    if (!t) return i;
    i.animation = t->animation;
    i.animator = t->animator;
    i.name = t->name;
    i.length = t->clip ? t->clip->length() : 0.0f;
    i.timePosition = t->time;
    i.speed = t->speed;
    i.weightCurrent = t->weight;
    i.weightTarget = t->target;
    i.playing = t->playing && !t->stopping;
    i.looped = t->looped;
    i.priority = t->priority;
    return i;
}

void AnimationSystem::event(const Track& t, AnimEvent::Kind kind, const std::string& name, const std::string& value) {
    AnimEvent e;
    e.kind = kind;
    e.track = t.id;
    e.animator = t.animator;
    e.owner = t.owner;
    e.name = name;
    e.value = value;
    events_.push_back(std::move(e));
}

void AnimationSystem::play(uint64_t id, float fadeTime, float weight, float speed) {
    Track* t = find(id);
    if (!t) return;
    t->time = speed < 0.0f && t->clip ? t->clip->length() : 0.0f; // Play restarts the animation
    t->speed = speed;
    t->playing = true;
    t->stopping = false;
    t->target = std::max(weight, 0.0f);
    if (fadeTime <= 0.0f) { t->weight = t->target; t->fadeRate = 0.0f; }
    else t->fadeRate = std::abs(t->target - t->weight) / fadeTime;
    event(*t, AnimEvent::Played);
    crossings(*t, -1.0f, t->time); // a keyframe right at the start
}

void AnimationSystem::stop(uint64_t id, float fadeTime) {
    Track* t = find(id);
    if (!t || !t->playing || t->stopping) return;
    t->stopping = true;
    t->target = 0.0f;
    if (fadeTime <= 0.0f) { t->weight = 0.0f; t->fadeRate = 0.0f; }
    else t->fadeRate = t->weight / fadeTime;
    event(*t, AnimEvent::Stopped);
}

void AnimationSystem::adjustSpeed(uint64_t id, float speed) {
    if (Track* t = find(id)) t->speed = speed;
}

void AnimationSystem::adjustWeight(uint64_t id, float weight, float fadeTime) {
    Track* t = find(id);
    if (!t) return;
    t->target = std::max(weight, 0.0f);
    if (fadeTime <= 0.0f) { t->weight = t->target; t->fadeRate = 0.0f; }
    else t->fadeRate = std::abs(t->target - t->weight) / fadeTime;
}

void AnimationSystem::setTimePosition(uint64_t id, float time) {
    Track* t = find(id);
    if (t && t->clip) t->time = std::clamp(time, 0.0f, t->clip->length());
}

void AnimationSystem::setLooped(uint64_t id, bool looped) {
    if (Track* t = find(id)) t->looped = looped;
}

void AnimationSystem::setPriority(uint64_t id, AnimationPriority priority) {
    if (Track* t = find(id)) t->priority = priority;
}

float AnimationSystem::timeOfKeyframe(uint64_t id, const std::string& name) const {
    const Track* t = find(id);
    if (!t || !t->clip) return -1.0f;
    for (const AnimKeyframe& k : t->clip->keyframes) if (k.name == name) return k.time;
    return -1.0f;
}

std::vector<uint64_t> AnimationSystem::playingTracks(uint64_t animator) const {
    std::vector<uint64_t> out;
    for (uint64_t id : order_) {
        const Track* t = find(id);
        if (t && t->animator == animator && t->playing && !t->stopping) out.push_back(id);
    }
    return out;
}

void AnimationSystem::crossings(const Track& t, float from, float to) {
    if (!t.clip || to <= from) return;
    for (const AnimKeyframe& k : t.clip->keyframes)
        if (!k.name.empty() && k.time > from && k.time <= to) event(t, AnimEvent::KeyframeReached, k.name);
    for (const AnimMarker& m : t.clip->markers)
        if (m.time > from && m.time <= to) event(t, AnimEvent::Marker, m.name, m.value);
}

void AnimationSystem::update(float dt) {
    // Tracks whose Animator is gone (the character respawned) go with it.
    for (auto it = order_.begin(); it != order_.end();) {
        Track* t = find(*it);
        if (!t || !t->scene || !t->scene->find(t->animator)) {
            tracks_.erase(*it);
            it = order_.erase(it);
        } else {
            ++it;
        }
    }

    // Time and fades.
    for (uint64_t id : order_) {
        Track& t = tracks_[id];
        if (!t.playing) continue;
        if (t.weight != t.target) {
            const float step = t.fadeRate > 0.0f ? t.fadeRate * dt : 1e9f;
            t.weight = t.weight < t.target ? std::min(t.target, t.weight + step) : std::max(t.target, t.weight - step);
        }
        if (t.stopping && t.weight <= 0.0f) {
            t.playing = t.stopping = false;
            t.weight = 0.0f;
            event(t, AnimEvent::Ended);
            continue;
        }
        const float length = t.clip ? t.clip->length() : 0.0f;
        if (length <= 0.0f) continue;
        const float before = t.time;
        t.time += dt * t.speed;
        if (t.speed >= 0.0f) {
            if (t.time >= length) {
                if (t.looped) {
                    crossings(t, before, length);
                    t.time = std::fmod(t.time, length);
                    event(t, AnimEvent::DidLoop);
                    crossings(t, -1.0f, t.time);
                } else {
                    crossings(t, before, length);
                    t.time = length;
                    if (!t.stopping) stop(id, 0.1f); // the end: fade back out
                }
            } else {
                crossings(t, before, t.time);
            }
        } else if (t.time <= 0.0f) { // backwards
            if (t.looped) {
                t.time = std::fmod(t.time, length) + length;
                event(t, AnimEvent::DidLoop);
            } else {
                t.time = 0.0f;
                if (!t.stopping) stop(id, 0.1f);
            }
        }
    }

    // Blend into each rig's joints and solve them. Rigs with tracks loaded
    // but none playing go back to their rest pose.
    struct Rig { Scene* scene; Instance* model; std::vector<const Track*> tracks; };
    std::vector<Rig> rigs;
    for (uint64_t id : order_) {
        const Track& t = tracks_[id];
        Instance* animator = t.scene->find(t.animator);
        Instance* model = animator ? rigOf(*animator) : nullptr;
        if (!model) continue;
        auto it = std::find_if(rigs.begin(), rigs.end(), [&](const Rig& r) { return r.model == model; });
        if (it == rigs.end()) { rigs.push_back({ t.scene, model, {} }); it = rigs.end() - 1; }
        if (t.playing && t.weight > 0.0f) it->tracks.push_back(&t);
    }
    struct Part { int rank; float weight; AnimPose pose; };
    std::unordered_map<std::string, AnimPose> sampled;
    for (Rig& rig : rigs) {
        std::unordered_map<std::string, std::vector<Part>> contributions;
        for (const Track* t : rig.tracks) {
            sampleClip(*t->clip, t->time, sampled);
            for (const auto& [joint, pose] : sampled) contributions[joint].push_back({ priorityRank(t->priority), t->weight * pose.weight, pose });
        }
        for (auto& [name, motor] : rigJoints(*rig.model)) {
            AnimPose result;
            auto c = contributions.find(name);
            if (c != contributions.end()) {
                std::vector<Part>& parts = c->second;
                std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.rank < b.rank; });
                for (size_t i = 0; i < parts.size();) {
                    // One priority level: the weighted average of its poses...
                    size_t j = i;
                    float sum = 0.0f;
                    glm::vec3 pos(0.0f);
                    glm::quat rot(0.0f, 0.0f, 0.0f, 0.0f);
                    for (; j < parts.size() && parts[j].rank == parts[i].rank; ++j) {
                        const float w = std::max(parts[j].weight, 0.0f);
                        sum += w;
                        pos += parts[j].pose.position * w;
                        glm::quat q = parts[j].pose.rotation;
                        if (glm::dot(q, parts[i].pose.rotation) < 0.0f) q = -q;
                        rot += q * w;
                    }
                    if (sum > 1e-5f) {
                        // ...laid over the levels below, as much as its total weight says.
                        const float alpha = std::min(sum, 1.0f);
                        result.position = glm::mix(result.position, pos / sum, alpha);
                        result.rotation = glm::slerp(result.rotation, glm::normalize(rot), alpha);
                    }
                    i = j;
                }
            }
            motor->jointTransform = CFrameValue::fromMatrix(poseMatrix(result));
        }
        solveJoints(*rig.scene, *rig.model);
    }
}

std::vector<AnimEvent> AnimationSystem::takeEvents() {
    std::vector<AnimEvent> out;
    out.swap(events_);
    return out;
}

void AnimationSystem::dropOwner(int owner) {
    for (auto it = order_.begin(); it != order_.end();) {
        auto t = tracks_.find(*it);
        if (t != tracks_.end() && t->second.owner == owner) {
            tracks_.erase(t);
            it = order_.erase(it);
        } else {
            ++it;
        }
    }
}

void AnimationSystem::clear() {
    tracks_.clear();
    order_.clear();
    events_.clear();
}

} // namespace rift
