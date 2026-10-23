#include "physics/PhysicsWorld.h"
#include "render/Primitives.h"
#include "core/Log.h"
#include "core/TransformUtil.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <thread>

namespace rift {

namespace {

// ---------------------------------------------------------------- layers
namespace Layers {
    constexpr JPH::ObjectLayer Static = 0;     // anchored parts
    constexpr JPH::ObjectLayer Kinematic = 1;  // anchored parts that scripts move
    constexpr JPH::ObjectLayer Dynamic = 2;    // unanchored assemblies
    constexpr JPH::ObjectLayer Ghost = 3;      // CanCollide = false: found by queries only
    constexpr JPH::ObjectLayer Character = 4;  // the characters' inner bodies
    constexpr JPH::ObjectLayer Count = 5;
}
namespace BPLayers {
    constexpr JPH::BroadPhaseLayer NonMoving(0);
    constexpr JPH::BroadPhaseLayer Moving(1);
    constexpr JPH::BroadPhaseLayer Ghost(2);
    constexpr JPH::uint Count = 3;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (layer == Layers::Static) return BPLayers::NonMoving;
        if (layer == Layers::Ghost) return BPLayers::Ghost;
        return BPLayers::Moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BPLayers::NonMoving ? "NonMoving" : layer == BPLayers::Moving ? "Moving" : "Ghost";
    }
#endif
};

// Simulation: only dynamic bodies collide with anything (static/kinematic/
// character bodies never need contacts among themselves); ghosts never do.
class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::Ghost || b == Layers::Ghost) return false;
        return a == Layers::Dynamic || b == Layers::Dynamic;
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        if (layer == Layers::Ghost || bp == BPLayers::Ghost) return false;
        if (layer == Layers::Dynamic) return true;
        return bp == BPLayers::Moving; // static/kinematic/character only meet dynamic bodies
    }
};

// Queries: a bit mask of object layers.
class LayerMask final : public JPH::ObjectLayerFilter {
public:
    explicit LayerMask(uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return (mask_ >> layer) & 1u; }
private:
    uint32_t mask_;
};
constexpr uint32_t bit(JPH::ObjectLayer l) { return 1u << l; }
constexpr uint32_t kSolidMask = bit(Layers::Static) | bit(Layers::Kinematic) | bit(Layers::Dynamic);
constexpr uint32_t kAllMask = kSolidMask | bit(Layers::Ghost) | bit(Layers::Character);

// ---------------------------------------------------------------- Jolt process setup
void joltTrace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    LOG_INFO("Jolt", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expression, const char* message, const char* file, JPH::uint line) {
    LOG_ERROR("Jolt", std::string("assert ") + expression + (message ? std::string(" (") + message + ")" : "") + " at " + file + ":" + std::to_string(line));
    return false; // keep running
}
#endif

void ensureJolt() {
    static const bool initialised = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)initialised;
}

// ---------------------------------------------------------------- conversions
JPH::Vec3 toJ(const glm::vec3& v) { return { v.x, v.y, v.z }; }
glm::vec3 toG(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }
JPH::Quat toJ(const glm::quat& q) { return { q.x, q.y, q.z, q.w }; }
glm::quat toG(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

struct Frame {
    glm::vec3 pos{ 0.0f };
    glm::quat rot{ 1.0f, 0.0f, 0.0f, 0.0f };
};
Frame frameOf(const glm::mat4& m) {
    Frame f;
    f.pos = glm::vec3(m[3]);
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) { const float l = glm::length(r[i]); if (l > 1e-8f) r[i] /= l; }
    f.rot = glm::normalize(glm::quat_cast(r));
    return f;
}
glm::vec3 eulerOf(const glm::quat& q) {
    glm::vec3 p, r, s;
    TransformUtil::decompose(glm::mat4_cast(q), p, r, s);
    return r;
}
bool sameFrame(const Frame& a, const Frame& b) {
    return glm::length(a.pos - b.pos) < 1e-4f && std::abs(glm::dot(a.rot, b.rot)) > 1.0f - 1e-7f;
}

// Jolt kg; RIFT Density 1 = 100 kg per cubic stud (a 4x1x2 brick weighs 800 kg).
constexpr float kDensityScale = 100.0f;
constexpr float kMaxStep = 1.0f / 60.0f;
constexpr int kMaxSubSteps = 8;

// Collision shape of one part, with the part id as its user data (queries
// map hits back to parts with GetSubShapeUserData).
JPH::Ref<JPH::Shape> makePartShape(const Instance& part) {
    const glm::vec3 s = glm::max(glm::abs(part.size), glm::vec3(0.02f));
    const float minHalf = std::min({ s.x, s.y, s.z }) * 0.5f;
    const float convexRadius = std::min(0.05f, minHalf * 0.5f);
    const float density = std::max(part.density, 0.01f) * kDensityScale;

    auto hull = [&](PartShape shape) {
        JPH::Array<JPH::Vec3> points;
        for (const glm::vec3& p : hullPoints(shape)) points.push_back(toJ(p * s));
        JPH::ConvexHullShapeSettings settings(points, convexRadius);
        settings.SetDensity(density);
        return settings.Create();
    };

    JPH::ShapeSettings::ShapeResult result;
    const PartShape shape = part.cls == ClassId::Part ? part.shape : PartShape::Block;
    switch (shape) {
        case PartShape::Ball:
            if (std::abs(s.x - s.y) < 1e-4f && std::abs(s.x - s.z) < 1e-4f) {
                JPH::SphereShapeSettings settings(s.x * 0.5f);
                settings.SetDensity(density);
                result = settings.Create();
            } else {
                result = hull(PartShape::Ball); // ellipsoid
            }
            break;
        case PartShape::Cylinder:
            if (std::abs(s.x - s.z) < 1e-4f) {
                JPH::CylinderShapeSettings settings(s.y * 0.5f, s.x * 0.5f, std::min(convexRadius, s.y * 0.25f));
                settings.SetDensity(density);
                result = settings.Create();
            } else {
                result = hull(PartShape::Cylinder);
            }
            break;
        case PartShape::Wedge:
        case PartShape::CornerWedge:
            result = hull(shape);
            break;
        case PartShape::Block:
        default: {
            JPH::BoxShapeSettings settings(toJ(s * 0.5f), convexRadius);
            settings.SetDensity(density);
            result = settings.Create();
            break;
        }
    }
    if (result.HasError()) {
        LOG_WARN("Physics", "'" + part.name + "': " + result.GetError().c_str() + "; using a box");
        JPH::BoxShapeSettings settings(toJ(s * 0.5f), convexRadius);
        result = settings.Create();
    }
    JPH::Ref<JPH::Shape> out = result.Get();
    out->SetUserData(part.id);
    return out;
}

bool inWorkspace(const Scene& scene, const Instance* inst) {
    return inst && scene.serviceOf(inst) == &scene.workspace() && scene.isInTree(inst);
}

} // namespace

// ============================================================================
//  Character
// ============================================================================
class CharacterImpl final : public CharacterController {
public:
    CharacterImpl(JPH::PhysicsSystem& system, uint64_t rootId, const glm::vec3& feet, float radius, float height)
        : system_(system), rootId_(rootId), radius_(radius), height_(height) {
        const float halfCylinder = std::max(0.01f, height * 0.5f - radius);
        JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(halfCylinder, radius);
        capsule->SetUserData(rootId);
        // Origin at the feet: the capsule sits on top of it.
        shape_ = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, height * 0.5f, 0.0f), JPH::Quat::sIdentity(), capsule).Create().Get();

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = shape_;
        settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
        settings->mMaxStrength = 4000.0f;      // how hard it pushes dynamic parts
        settings->mMass = 70.0f;
        settings->mCharacterPadding = 0.02f;
        settings->mPenetrationRecoverySpeed = 1.0f;
        settings->mPredictiveContactDistance = 0.1f;
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius); // contacts on the lower hemisphere support it
        settings->mEnhancedInternalEdgeRemoval = true;
        settings->mInnerBodyShape = shape_;  // a body other parts and queries can see
        settings->mInnerBodyLayer = Layers::Character;
        character_ = new JPH::CharacterVirtual(settings, toJ(feet), JPH::Quat::sIdentity(), rootId, &system_);
    }

    void setInput(const Input& input) override { input_ = input; }
    glm::vec3 feetPosition() const override { return toG(character_->GetPosition()); }
    glm::vec3 velocity() const override { return toG(character_->GetLinearVelocity()); }
    void setVelocity(const glm::vec3& v) override { character_->SetLinearVelocity(toJ(v)); }
    bool onGround() const override { return character_->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround; }
    bool justJumped() const override { return jumped_; }
    void teleport(const glm::vec3& feet) override {
        character_->SetPosition(toJ(feet));
        character_->SetLinearVelocity(JPH::Vec3::sZero());
    }
    uint64_t rootPartId() const override { return rootId_; }
    float radius() const override { return radius_; }
    float height() const override { return height_; }

    void resetStepFlags() { jumped_ = false; }

    // Jolt's recommended CharacterVirtual update (see its CharacterVirtualTest).
    void update(float dt, JPH::TempAllocator& temp) {
        const JPH::Vec3 gravity = system_.GetGravity();
        character_->UpdateGroundVelocity();
        const JPH::Vec3 up = JPH::Vec3::sAxisY();
        const JPH::Vec3 currentVertical = character_->GetLinearVelocity().Dot(up) * up;
        const JPH::Vec3 groundVelocity = character_->GetGroundVelocity();
        const bool movingTowardsGround = (currentVertical.GetY() - groundVelocity.GetY()) < 0.1f;

        JPH::Vec3 v;
        if (character_->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && movingTowardsGround
            && !character_->IsSlopeTooSteep(character_->GetGroundNormal())) {
            v = groundVelocity; // ride moving platforms
            if (input_.jump) {
                v += input_.jumpSpeed * up;
                jumped_ = true;
            }
        } else {
            v = currentVertical;
        }
        v += gravity * dt;
        glm::vec3 move = input_.moveDirection;
        move.y = 0.0f;
        if (glm::length(move) > 1.0f) move = glm::normalize(move);
        v += toJ(move * input_.walkSpeed);
        character_->SetLinearVelocity(v);

        JPH::CharacterVirtual::ExtendedUpdateSettings ext;
        ext.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.5f, 0.0f);
        ext.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.45f, 0.0f);
        const LayerMask layers(kSolidMask);
        character_->ExtendedUpdate(dt, gravity, ext, JPH::BroadPhaseLayerFilter(), layers, JPH::BodyFilter(), JPH::ShapeFilter(), temp);
    }

    const JPH::Shape* shape() const { return shape_; }
    JPH::BodyID innerBody() const { return character_->GetInnerBodyID(); }

private:
    JPH::PhysicsSystem& system_;
    JPH::Ref<JPH::CharacterVirtual> character_;
    JPH::RefConst<JPH::Shape> shape_;
    uint64_t rootId_;
    float radius_, height_;
    Input input_;
    bool jumped_ = false;
};

// ============================================================================
//  Impl
// ============================================================================
struct PhysicsWorld::Impl {
    Scene& scene;
    BroadPhaseLayers bpLayers;
    ObjectVsBroadPhaseFilter objVsBp;
    ObjectPairFilter pairFilter;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;

    enum class Kind { Static, Kinematic, Dynamic, Ghost };
    struct BodyRec {
        JPH::BodyID body;
        Kind kind = Kind::Static;
        std::vector<uint64_t> members; // parts in this body (the owner first)
        Frame last;                    // owner frame after the last sync
        glm::vec3 lastVelocity{ 0.0f }, lastAngular{ 0.0f };
        bool fallen = false;
    };
    std::unordered_map<uint64_t, BodyRec> bodies;   // by owner part id (dynamic root or standalone part)
    std::unordered_map<uint64_t, BodyRec> ghosts;   // non-colliding members of dynamic assemblies, by part id
    std::unordered_map<uint64_t, uint64_t> ownerOf; // part id -> owner key in `bodies`
    std::unordered_set<uint64_t> dirty;             // parts whose bodies must be recomputed
    bool fullRebuild = true;

    std::unordered_set<uint64_t> ignored;           // subtrees without bodies (characters)
    std::unordered_set<uint64_t> watched;
    std::set<std::pair<uint64_t, uint64_t>> touching;
    std::vector<TouchEvent> touchEvents;
    std::vector<uint64_t> fallen;
    std::vector<std::unique_ptr<CharacterImpl>> characters;

    explicit Impl(Scene& s) : scene(s) {
        ensureJolt();
        temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
        const int threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, std::min(threads, 8));
        system = std::make_unique<JPH::PhysicsSystem>();
        system->Init(65536, 0, 65536, 16384, bpLayers, objVsBp, pairFilter);
        JPH::PhysicsSettings settings = system->GetPhysicsSettings();
        settings.mAllowSleeping = true;
        system->SetPhysicsSettings(settings);
    }

    ~Impl() {
        characters.clear();
        JPH::BodyInterface& bi = system->GetBodyInterface();
        for (auto* map : { &bodies, &ghosts }) {
            for (auto& [id, rec] : *map) { bi.RemoveBody(rec.body); bi.DestroyBody(rec.body); }
            map->clear();
        }
    }

    JPH::BodyInterface& bi() { return system->GetBodyInterface(); }

    // ---------------------------------------------------------- assemblies
    // The dynamic root a part belongs to: the highest unanchored part in its
    // chain of part ancestors (child parts are welded to moving parents).
    // A part with no unanchored ancestor-or-self is standalone (returns itself).
    Instance* rootOf(Instance* part) const {
        Instance* root = nullptr;
        for (Instance* p = part; p && p->isBasePart(); p = p->parent) {
            if (!p->anchored) root = p;
        }
        return root ? root : part;
    }
    bool isDynamicRoot(const Instance* part) const {
        return part->isBasePart() && !part->anchored && rootOf(const_cast<Instance*>(part)) == part;
    }
    void collectMembers(Instance* root, std::vector<Instance*>& out) const {
        out.push_back(root);
        for (Instance* c : root->children) if (c->isBasePart()) collectMembers(c, out);
    }

    void removeBody(std::unordered_map<uint64_t, BodyRec>& map, uint64_t key) {
        auto it = map.find(key);
        if (it == map.end()) return;
        bi().RemoveBody(it->second.body);
        bi().DestroyBody(it->second.body);
        if (&map == &bodies) {
            for (uint64_t m : it->second.members) {
                auto o = ownerOf.find(m);
                if (o != ownerOf.end() && o->second == key) ownerOf.erase(o);
                removeBody(ghosts, m);
            }
        }
        map.erase(it);
    }

    JPH::BodyID createBody(const JPH::Shape* shape, const Frame& f, JPH::EMotionType motion, JPH::ObjectLayer layer, const Instance& owner) {
        JPH::BodyCreationSettings bcs(shape, toJ(f.pos), toJ(f.rot), motion, layer);
        bcs.mAllowDynamicOrKinematic = true;
        bcs.mFriction = owner.friction;
        bcs.mRestitution = owner.elasticity;
        bcs.mUserData = owner.id;
        if (motion == JPH::EMotionType::Dynamic) {
            bcs.mMotionQuality = JPH::EMotionQuality::LinearCast; // no tunnelling through thin parts
            bcs.mLinearVelocity = toJ(owner.velocity);
            bcs.mAngularVelocity = toJ(glm::radians(owner.angularVelocity));
            bcs.mLinearDamping = 0.05f;
            bcs.mAngularDamping = 0.1f;
        }
        const JPH::BodyID id = bi().CreateAndAddBody(bcs, motion == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        if (id.IsInvalid()) LOG_ERROR("Physics", "Body limit reached; '" + owner.name + "' has no physics");
        return id;
    }

    bool isIgnored(const Instance* part) const {
        if (ignored.empty()) return false;
        for (const Instance* p = part; p; p = p->parent) if (ignored.count(p->id)) return true;
        return false;
    }

    void buildFor(Instance* part) {
        if (isIgnored(part)) return;
        Instance* root = rootOf(part);
        if (bodies.count(root->id)) return; // built already (another member asked first)
        if (!root->anchored) buildDynamic(root);
        else buildStatic(root);
    }

    void buildStatic(Instance* part) {
        BodyRec rec;
        rec.kind = Kind::Static;
        rec.members = { part->id };
        rec.last = frameOf(part->worldFrame());
        JPH::Ref<JPH::Shape> shape = makePartShape(*part);
        rec.body = createBody(shape, rec.last, JPH::EMotionType::Static, part->canCollide ? Layers::Static : Layers::Ghost, *part);
        if (rec.body.IsInvalid()) return;
        ownerOf[part->id] = part->id;
        bodies.emplace(part->id, std::move(rec));
    }

    void buildDynamic(Instance* root) {
        std::vector<Instance*> members;
        collectMembers(root, members);
        const Frame rootFrame = frameOf(root->worldFrame());
        const glm::mat4 rootInverse = glm::inverse(root->worldFrame());

        JPH::StaticCompoundShapeSettings compound;
        int colliding = 0;
        for (Instance* m : members) if (m->canCollide) ++colliding;
        const bool solid = colliding > 0;
        JPH::Ref<JPH::Shape> single;
        for (Instance* m : members) {
            if (solid && !m->canCollide) continue;
            JPH::Ref<JPH::Shape> leaf = makePartShape(*m);
            if (m == root && (members.size() == 1 || (solid && colliding == 1))) single = leaf;
            const Frame rel = frameOf(rootInverse * m->worldFrame());
            compound.AddShape(toJ(rel.pos), toJ(rel.rot), leaf);
        }
        JPH::Ref<JPH::Shape> shape = single;
        if (!shape) {
            auto result = compound.Create();
            if (result.HasError()) { LOG_WARN("Physics", "'" + root->name + "': " + result.GetError().c_str()); return; }
            shape = result.Get();
        }

        BodyRec rec;
        rec.kind = Kind::Dynamic;
        rec.last = rootFrame;
        rec.lastVelocity = root->velocity;
        rec.lastAngular = root->angularVelocity;
        rec.body = createBody(shape, rootFrame, JPH::EMotionType::Dynamic, solid ? Layers::Dynamic : Layers::Ghost, *root);
        if (rec.body.IsInvalid()) return;
        for (Instance* m : members) {
            rec.members.push_back(m->id);
            ownerOf[m->id] = root->id;
        }
        bodies.emplace(root->id, std::move(rec));

        // Non-colliding members still need to be found by Touched and raycasts.
        if (solid) {
            for (Instance* m : members) {
                if (m->canCollide) continue;
                BodyRec ghost;
                ghost.kind = Kind::Ghost;
                ghost.members = { m->id };
                ghost.last = frameOf(m->worldFrame());
                JPH::Ref<JPH::Shape> leaf = makePartShape(*m);
                ghost.body = createBody(leaf, ghost.last, JPH::EMotionType::Kinematic, Layers::Ghost, *m);
                if (!ghost.body.IsInvalid()) ghosts.emplace(m->id, std::move(ghost));
            }
        }
    }

    void rebuildAll() {
        for (auto* map : { &ghosts, &bodies }) {
            for (auto& [id, rec] : *map) { bi().RemoveBody(rec.body); bi().DestroyBody(rec.body); }
            map->clear();
        }
        ownerOf.clear();
        std::vector<Instance*> parts;
        Scene::forEachDescendant(scene.workspace(), [&](Instance& i) { if (i.isBasePart()) parts.push_back(&i); });
        for (Instance* p : parts) buildFor(p);
        fullRebuild = false;
        dirty.clear();
    }

    void processDirty() {
        if (fullRebuild) { rebuildAll(); return; }
        if (dirty.empty()) return;
        // Every body that contains a dirty part (or one of its descendants)
        // is rebuilt, and so is the body each of them belongs to now.
        std::unordered_set<uint64_t> owners;
        std::vector<Instance*> affected;
        for (uint64_t id : dirty) {
            auto o = ownerOf.find(id);
            if (o != ownerOf.end()) owners.insert(o->second);
            if (ghosts.count(id)) removeBody(ghosts, id);
            Instance* inst = scene.find(id);
            if (!inst) continue;
            affected.push_back(inst);
            Scene::forEachDescendant(*inst, [&](Instance& d) {
                auto od = ownerOf.find(d.id);
                if (od != ownerOf.end()) owners.insert(od->second);
                if (d.isBasePart()) affected.push_back(&d);
            });
        }
        dirty.clear();
        std::vector<Instance*> rebuild;
        for (uint64_t owner : owners) {
            auto it = bodies.find(owner);
            if (it == bodies.end()) continue;
            for (uint64_t m : it->second.members) if (Instance* mi = scene.find(m)) rebuild.push_back(mi);
            removeBody(bodies, owner);
        }
        for (Instance* p : affected) rebuild.push_back(p);
        for (Instance* p : rebuild) {
            if (p->isBasePart() && inWorkspace(scene, p) && !ownerOf.count(p->id)) buildFor(p);
        }
    }

    // ---------------------------------------------------------- sync
    // Before stepping: apply what scripts / the editor changed since the last
    // step (teleports, velocities), and start moving anchored parts whose
    // place changed (kinematic).
    struct KinematicMove { JPH::BodyID body; Frame from, to; };
    std::vector<KinematicMove> kinematicMoves;

    void syncFromScene() {
        kinematicMoves.clear();
        for (auto& [ownerId, rec] : bodies) {
            Instance* owner = scene.find(ownerId);
            if (!owner) continue;
            const Frame now = frameOf(owner->worldFrame());
            if (rec.kind == Kind::Dynamic) {
                if (!sameFrame(now, rec.last)) {
                    bi().SetPositionAndRotation(rec.body, toJ(now.pos), toJ(now.rot), JPH::EActivation::Activate);
                    rec.last = now;
                }
                if (glm::length(owner->velocity - rec.lastVelocity) > 1e-4f) {
                    bi().SetLinearVelocity(rec.body, toJ(owner->velocity));
                    rec.lastVelocity = owner->velocity;
                }
                if (glm::length(owner->angularVelocity - rec.lastAngular) > 1e-3f) {
                    bi().SetAngularVelocity(rec.body, toJ(glm::radians(owner->angularVelocity)));
                    rec.lastAngular = owner->angularVelocity;
                }
            } else {
                const bool moved = !sameFrame(now, rec.last);
                if (moved && rec.kind == Kind::Static) {
                    // Becomes kinematic for good: it is being animated by a script.
                    bi().SetMotionType(rec.body, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
                    if (owner->canCollide) bi().SetObjectLayer(rec.body, Layers::Kinematic);
                    rec.kind = Kind::Kinematic;
                }
                if (rec.kind == Kind::Kinematic) kinematicMoves.push_back({ rec.body, rec.last, now });
                rec.last = now;
            }
        }
    }

    void moveKinematics(int subStep, int subSteps, float h) {
        const float t = static_cast<float>(subStep + 1) / static_cast<float>(subSteps);
        for (const KinematicMove& k : kinematicMoves) {
            const glm::vec3 p = glm::mix(k.from.pos, k.to.pos, t);
            const glm::quat r = glm::slerp(k.from.rot, k.to.rot, t);
            bi().MoveKinematic(k.body, toJ(p), toJ(r), h);
        }
    }

    void writeBack() {
        const float fallHeight = scene.workspace().fallenPartsHeight;
        for (auto& [ownerId, rec] : bodies) {
            if (rec.kind != Kind::Dynamic) continue;
            Instance* owner = scene.find(ownerId);
            if (!owner) continue;
            JPH::RVec3 pos;
            JPH::Quat rot;
            bi().GetPositionAndRotation(rec.body, pos, rot);
            rec.last.pos = toG(pos);
            rec.last.rot = toG(rot);
            owner->setWorldFrame(rec.last.pos, eulerOf(rec.last.rot));
            rec.lastVelocity = owner->velocity = toG(bi().GetLinearVelocity(rec.body));
            rec.lastAngular = owner->angularVelocity = glm::degrees(toG(bi().GetAngularVelocity(rec.body)));
            if (rec.last.pos.y < fallHeight && !rec.fallen) {
                rec.fallen = true;
                fallen.push_back(ownerId);
            }
        }
        // Ghost bodies follow their parts.
        for (auto& [partId, rec] : ghosts) {
            Instance* p = scene.find(partId);
            if (!p) continue;
            const Frame now = frameOf(p->worldFrame());
            if (!sameFrame(now, rec.last)) {
                bi().SetPositionAndRotation(rec.body, toJ(now.pos), toJ(now.rot), JPH::EActivation::DontActivate);
                rec.last = now;
            }
        }
    }

    // ---------------------------------------------------------- touches
    bool sameAssembly(uint64_t a, uint64_t b) const {
        auto oa = ownerOf.find(a), ob = ownerOf.find(b);
        return oa != ownerOf.end() && ob != ownerOf.end() && oa->second == ob->second;
    }

    CharacterImpl* characterFor(uint64_t rootId) const {
        for (const auto& c : characters) if (c->rootPartId() == rootId) return c.get();
        return nullptr;
    }

    std::vector<uint64_t> queryTouching(uint64_t partId) const {
        std::vector<uint64_t> out;
        const Instance* part = scene.find(partId);
        JPH::RefConst<JPH::Shape> shape;
        JPH::RMat44 transform;
        JPH::BodyID self;
        if (CharacterImpl* c = characterFor(partId)) {
            shape = c->shape();
            transform = JPH::RMat44::sTranslation(toJ(c->feetPosition()));
            self = c->innerBody();
        } else {
            if (!part || !part->isBasePart() || !part->canTouch || !inWorkspace(scene, part)) return out;
            shape = makePartShape(*part);
            const Frame f = frameOf(part->worldFrame());
            transform = JPH::RMat44::sRotationTranslation(toJ(f.rot), toJ(f.pos));
        }
        JPH::CollideShapeSettings settings;
        settings.mMaxSeparationDistance = 0.05f; // resting contact counts as touching
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        const LayerMask layers(kAllMask);
        JPH::IgnoreSingleBodyFilter bodyFilter(self);
        system->GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(), transform, settings, JPH::RVec3::sZero(), collector,
                                                  JPH::BroadPhaseLayerFilter(), layers, bodyFilter);
        std::unordered_set<uint64_t> seen;
        for (const JPH::CollideShapeResult& hit : collector.mHits) {
            JPH::BodyLockRead lock(system->GetBodyLockInterfaceNoLock(), hit.mBodyID2);
            if (!lock.Succeeded()) continue;
            const uint64_t other = lock.GetBody().GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
            if (other == 0 || other == partId || seen.count(other)) continue;
            if (sameAssembly(partId, other)) continue;
            const Instance* o = scene.find(other);
            if (!o || (o->isBasePart() && !o->canTouch)) continue;
            seen.insert(other);
            out.push_back(other);
        }
        return out;
    }

    void detectTouches() {
        std::set<std::pair<uint64_t, uint64_t>> now;
        for (uint64_t id : watched) {
            for (uint64_t other : queryTouching(id)) now.insert({ id, other });
        }
        for (const auto& pair : now) if (!touching.count(pair)) touchEvents.push_back({ pair.first, pair.second, true });
        for (const auto& pair : touching) if (!now.count(pair)) touchEvents.push_back({ pair.first, pair.second, false });
        touching = std::move(now);
    }

    JPH::BodyID bodyOfPart(uint64_t partId) const {
        auto o = ownerOf.find(partId);
        if (o == ownerOf.end()) return {};
        auto it = bodies.find(o->second);
        return it == bodies.end() ? JPH::BodyID() : it->second.body;
    }
};

// ============================================================================
//  PhysicsWorld
// ============================================================================
PhysicsWorld::PhysicsWorld(Scene& scene) : impl_(std::make_unique<Impl>(scene)) {
    scene.addObserver(this);
}

PhysicsWorld::~PhysicsWorld() {
    impl_->scene.removeObserver(this);
}

void PhysicsWorld::step(float dt) {
    Impl& s = *impl_;
    if (dt <= 0.0f) return;
    s.processDirty();
    s.system->SetGravity(JPH::Vec3(0.0f, -s.scene.workspace().gravity, 0.0f));
    s.syncFromScene();

    const int steps = std::clamp(static_cast<int>(std::ceil(dt / kMaxStep)), 1, kMaxSubSteps);
    const float h = dt / static_cast<float>(steps);
    for (auto& c : s.characters) c->resetStepFlags();
    for (int i = 0; i < steps; ++i) {
        s.moveKinematics(i, steps, h);
        for (auto& c : s.characters) c->update(h, *s.temp);
        s.system->Update(h, 1, s.temp.get(), s.jobs.get());
    }
    s.writeBack();
    s.detectTouches();
}

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, const RaycastFilter& filter) const {
    impl_->processDirty(); // parts created since the last step must be hittable
    const Impl& s = *impl_;
    if (glm::length(direction) < 1e-6f) return std::nullopt;

    std::unordered_set<uint64_t> listed(filter.instances.begin(), filter.instances.end());
    const Scene& scene = s.scene;
    auto listedOrAncestor = [&](uint64_t id) {
        for (const Instance* i = scene.find(id); i; i = i->parent) if (listed.count(i->id)) return true;
        return false;
    };
    class InstanceShapeFilter final : public JPH::ShapeFilter {
    public:
        std::function<bool(uint64_t)> accept;
        bool ShouldCollide(const JPH::Shape* shape, const JPH::SubShapeID&) const override { return accept(shape->GetUserData()); }
        bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape* shape2, const JPH::SubShapeID&) const override { return accept(shape2->GetUserData()); }
    } shapeFilter;
    shapeFilter.accept = [&](uint64_t id) {
        if (id == 0) return true;
        if (listed.empty()) return !filter.include;
        return listedOrAncestor(id) == filter.include;
    };

    const JPH::RRayCast ray{ toJ(origin), toJ(direction) };
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    const LayerMask layers(filter.respectCanCollide ? (kSolidMask | bit(Layers::Character)) : kAllMask);
    JPH::RayCastSettings settings;
    s.system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, JPH::BroadPhaseLayerFilter(), layers, JPH::BodyFilter(), shapeFilter);
    if (!collector.HadHit()) return std::nullopt;

    const JPH::RayCastResult& hit = collector.mHit;
    JPH::BodyLockRead lock(s.system->GetBodyLockInterfaceNoLock(), hit.mBodyID);
    if (!lock.Succeeded()) return std::nullopt;
    const JPH::Body& body = lock.GetBody();
    RaycastHit out;
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out.instance = body.GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
    out.position = toG(point);
    out.normal = toG(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
    out.distance = hit.mFraction * glm::length(direction);
    return out;
}

std::vector<uint64_t> PhysicsWorld::partsInBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& rotation) const {
    impl_->processDirty();
    const Impl& s = *impl_;
    std::vector<uint64_t> out;
    JPH::Ref<JPH::Shape> box = new JPH::BoxShape(toJ(glm::max(size * 0.5f, glm::vec3(0.01f))), 0.0f);
    const glm::quat q = glm::quat(glm::radians(rotation));
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const LayerMask layers(kAllMask);
    s.system->GetNarrowPhaseQuery().CollideShape(box, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(toJ(q), toJ(center)),
                                                JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector, JPH::BroadPhaseLayerFilter(), layers);
    std::unordered_set<uint64_t> seen;
    for (const auto& hit : collector.mHits) {
        JPH::BodyLockRead lock(s.system->GetBodyLockInterfaceNoLock(), hit.mBodyID2);
        if (!lock.Succeeded()) continue;
        const uint64_t id = lock.GetBody().GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
        if (id && seen.insert(id).second) out.push_back(id);
    }
    return out;
}

void PhysicsWorld::watchTouches(uint64_t partId, bool watch) {
    if (watch) impl_->watched.insert(partId);
    else impl_->watched.erase(partId);
}

std::vector<TouchEvent> PhysicsWorld::takeTouchEvents() {
    std::vector<TouchEvent> out;
    out.swap(impl_->touchEvents);
    return out;
}

std::vector<uint64_t> PhysicsWorld::touchingParts(uint64_t partId) const {
    impl_->processDirty();
    return impl_->queryTouching(partId);
}

std::vector<uint64_t> PhysicsWorld::takeFallenParts() {
    std::vector<uint64_t> out;
    out.swap(impl_->fallen);
    return out;
}

void PhysicsWorld::applyImpulse(uint64_t partId, const glm::vec3& impulse) {
    const JPH::BodyID body = impl_->bodyOfPart(partId);
    if (!body.IsInvalid()) impl_->bi().AddImpulse(body, toJ(impulse));
}

void PhysicsWorld::applyAngularImpulse(uint64_t partId, const glm::vec3& impulse) {
    const JPH::BodyID body = impl_->bodyOfPart(partId);
    if (!body.IsInvalid()) impl_->bi().AddAngularImpulse(body, toJ(impulse));
}

float PhysicsWorld::massOf(uint64_t partId) const {
    const JPH::BodyID body = impl_->bodyOfPart(partId);
    if (body.IsInvalid()) return 0.0f;
    JPH::BodyLockRead lock(impl_->system->GetBodyLockInterfaceNoLock(), body);
    if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0.0f;
    const float inv = lock.GetBody().GetMotionProperties()->GetInverseMass();
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

CharacterController* PhysicsWorld::createCharacter(uint64_t rootPartId, const glm::vec3& feetPosition, float radius, float height) {
    impl_->processDirty(); // the character's own parts must not be bodies themselves
    auto c = std::make_unique<CharacterImpl>(*impl_->system, rootPartId, feetPosition, radius, height);
    CharacterController* raw = c.get();
    impl_->characters.push_back(std::move(c));
    return raw;
}

void PhysicsWorld::destroyCharacter(CharacterController* character) {
    auto& list = impl_->characters;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& c) { return c.get() == character; }), list.end());
}

void PhysicsWorld::setIgnored(uint64_t instanceId, bool ignored) {
    if (ignored) impl_->ignored.insert(instanceId);
    else impl_->ignored.erase(instanceId);
    impl_->dirty.insert(instanceId);
}

size_t PhysicsWorld::bodyCount() const { return impl_->bodies.size() + impl_->ghosts.size(); }

// ---------------------------------------------------------------- observer
void PhysicsWorld::onInstanceAdded(Instance& instance) {
    if (instance.isBasePart() || !instance.children.empty()) impl_->dirty.insert(instance.id);
}

void PhysicsWorld::onInstanceRemoving(Instance& instance) {
    Impl& s = *impl_;
    // The instance dies right after this call: its bodies go now, and the
    // body it was welded into is rebuilt at the next step.
    auto o = s.ownerOf.find(instance.id);
    if (o != s.ownerOf.end()) {
        const uint64_t owner = o->second;
        auto it = s.bodies.find(owner);
        if (it != s.bodies.end()) {
            for (uint64_t m : it->second.members) if (m != instance.id && m != owner) s.dirty.insert(m);
            if (owner != instance.id) s.dirty.insert(owner);
        }
        s.removeBody(s.bodies, owner);
    }
    s.removeBody(s.ghosts, instance.id);
    s.watched.erase(instance.id);
    s.dirty.erase(instance.id);
    for (auto it = s.touching.begin(); it != s.touching.end();) {
        if (it->first == instance.id || it->second == instance.id) it = s.touching.erase(it); else ++it;
    }
}

void PhysicsWorld::onParentChanged(Instance& instance, Instance*) {
    impl_->dirty.insert(instance.id);
}

void PhysicsWorld::onPropertyChanged(Instance& instance, const PropertyInfo& prop) {
    if (!instance.isBasePart()) return;
    static const char* kRebuild[] = { "Size", "Shape", "Anchored", "CanCollide", "Density", "Friction", "Elasticity", "MeshId" };
    for (const char* name : kRebuild) {
        if (prop.name == name) { impl_->dirty.insert(instance.id); return; }
    }
    // Position/Rotation of a welded (non-root) member changes the assembly's shape.
    if ((prop.name == "Position" || prop.name == "Rotation")) {
        auto o = impl_->ownerOf.find(instance.id);
        if (o != impl_->ownerOf.end() && o->second != instance.id) impl_->dirty.insert(instance.id);
    }
}

} // namespace rift
