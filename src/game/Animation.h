#pragma once
//
// Animation.h -- Roblox's animation system: Animator (or AnimationController),
// Animation (AnimationId), KeyframeSequence clips, AnimationTracks and
// Motor6D joints.
//
//   * A clip (AnimClip) is a list of keyframes, each with a pose (an offset
//     CFrame) per joint, named after the joint's Part1 ("Right Arm"). Clips
//     come from .anim files (the Animation Editor saves them; the default
//     character animations are in data/animations) or from a KeyframeSequence
//     in the scene ("kfs:<id>", KeyframeSequenceProvider).
//   * Tracks play clips on an Animator's rig: fading in and out, looping,
//     speed and weight. Every frame the tracks of a rig are blended (higher
//     Priority overrides lower on the joints it moves; equal priorities mix
//     by weight) into each Motor6D's Transform, and the joints are solved:
//         Part1 = Part0 * C0 * Transform * C1:Inverse()
//   * The system belongs to the GameSession and works on the scene that has
//     the rig: the server's world for characters (so every client sees the
//     limbs move), a client's own world for models only it has.
//
#include "scene/Scene.h"

#include <nlohmann/json_fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rift {

// ---------------------------------------------------------------- clips
struct AnimPose {
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    float weight = 1.0f;
    PoseEasingStyle style = PoseEasingStyle::Linear;       // towards the next keyframe
    PoseEasingDirection direction = PoseEasingDirection::In;
};

struct AnimKeyframe {
    float time = 0.0f;
    std::string name;                                      // KeyframeReached fires with it ("" = none)
    std::unordered_map<std::string, AnimPose> poses;       // by joint (Part1) name
};

struct AnimMarker {
    float time = 0.0f;
    std::string name, value;
};

struct AnimClip {
    std::string name;
    std::vector<AnimKeyframe> keyframes;                   // sorted by time
    std::vector<AnimMarker> markers;                       // sorted by time
    bool loop = false;
    AnimationPriority priority = AnimationPriority::Core;
    float length() const { return keyframes.empty() ? 0.0f : keyframes.back().time; }
    // Sorts keyframes and markers and builds `channels`; call after changing keyframes.
    void prepare();
    // Per joint: (time, pose) of the keyframes that pose it, sorted by time.
    std::unordered_map<std::string, std::vector<std::pair<float, AnimPose>>> channels;
};

// .anim files (JSON) and KeyframeSequences.
std::shared_ptr<AnimClip> clipFromJson(const nlohmann::json& j, std::string* error = nullptr);
nlohmann::json clipToJson(const AnimClip& clip);
std::shared_ptr<AnimClip> clipFromSequence(const Instance& keyframeSequence);
// Fills an (empty) KeyframeSequence with Keyframes, Poses and markers.
void clipToSequence(const AnimClip& clip, Scene& scene, Instance& keyframeSequence);
bool saveClip(const AnimClip& clip, const std::filesystem::path& file, std::string* error = nullptr);
std::shared_ptr<AnimClip> loadClipFile(const std::filesystem::path& file, std::string* error = nullptr);

// The pose of every joint the clip moves at `time` (weights included).
void sampleClip(const AnimClip& clip, float time, std::unordered_map<std::string, AnimPose>& out);
float easePose(PoseEasingStyle style, PoseEasingDirection direction, float t);
int priorityRank(AnimationPriority p); // blending order: Core lowest

// ---------------------------------------------------------------- joints
glm::mat4 poseMatrix(const AnimPose& pose);
AnimPose poseFromMatrix(const glm::mat4& m);
// Moves every Motor6D's Part1 inside `root` (its whole subtree), parents first.
void solveJoints(Scene& scene, Instance& root);
// The Motor6Ds of a rig, by the name of the part they move.
std::unordered_map<std::string, Instance*> rigJoints(Instance& rig);
// The model an Animator works on (its Humanoid's / AnimationController's parent).
Instance* rigOf(const Instance& animator);

// Enum.HumanoidStateType, in the Lua enum's order.
enum class HumanoidState : int {
    FallingDown, Running, RunningNoPhysics, Climbing, StrafingNoPhysics, Ragdoll, GettingUp, Jumping,
    Landed, Flying, Freefall, Seated, PlatformStanding, Dead, Swimming, Physics, None
};

// The default character: a HumanoidRootPart, Torso, Head (with eyes), arms
// and legs joined by Motor6Ds (RootJoint, Neck, Left/Right Shoulder,
// Left/Right Hip), and a Humanoid with an Animator. Not parented.
struct RigColors { Color3 shirt{ 0.2f, 0.45f, 0.85f }, skin{ 0.98f, 0.82f, 0.55f }, pants{ 0.17f, 0.20f, 0.28f }; };
Instance* buildCharacterRig(Scene& scene, const RigColors& colors, bool anchored);

// ---------------------------------------------------------------- tracks
struct AnimEvent {
    enum Kind { Played, Stopped, Ended, DidLoop, KeyframeReached, Marker };
    Kind kind = Played;
    uint64_t track = 0;
    uint64_t animator = 0;
    int owner = -1;             // who loaded the track: -1 the server, else a client index
    std::string name, value;    // keyframe / marker name, marker value
};

struct TrackInfo {
    uint64_t animation = 0;     // the Animation instance it was loaded from
    uint64_t animator = 0;
    std::string name;           // the Animation's name
    float length = 0.0f, timePosition = 0.0f, speed = 1.0f;
    float weightCurrent = 0.0f, weightTarget = 0.0f;
    bool playing = false, looped = false;
    AnimationPriority priority = AnimationPriority::Core;
};

class AnimationSystem {
public:
    // Where tracks run: the server's scene and each client's (index 0..).
    void setScenes(Scene* server, std::vector<Scene*> clients) { server_ = server; clients_ = std::move(clients); }

    // animator:LoadAnimation(animation). `owner` is the side calling it (-1
    // server, else the client's index); `from` is that side's scene. Returns
    // the new track's id, or 0 with `error` set.
    uint64_t load(int owner, Scene& from, const Instance& animator, const Instance& animation, std::string& error);

    bool exists(uint64_t track) const { return tracks_.count(track) != 0; }
    TrackInfo info(uint64_t track) const;
    void play(uint64_t track, float fadeTime = 0.1f, float weight = 1.0f, float speed = 1.0f);
    void stop(uint64_t track, float fadeTime = 0.1f);
    void adjustSpeed(uint64_t track, float speed);
    void adjustWeight(uint64_t track, float weight, float fadeTime = 0.1f);
    void setTimePosition(uint64_t track, float time);
    void setLooped(uint64_t track, bool looped);
    void setPriority(uint64_t track, AnimationPriority priority);
    float timeOfKeyframe(uint64_t track, const std::string& name) const; // -1 if none
    std::vector<uint64_t> playingTracks(uint64_t animator) const;

    // Per frame: moves tracks, blends them into the Motor6Ds and solves the rigs.
    void update(float dt);
    std::vector<AnimEvent> takeEvents();
    // A side's scripts stopped (or its player left): its tracks go.
    void dropOwner(int owner);
    void clear();

    // Clips by AnimationId, cached (files are read once).
    std::shared_ptr<AnimClip> clipFor(const std::string& animationId, Scene& from, std::string& error);

private:
    struct Track {
        uint64_t id = 0;
        int owner = -1;
        Scene* scene = nullptr;                  // where the rig is
        uint64_t animator = 0, animation = 0;
        std::string name;
        std::shared_ptr<AnimClip> clip;
        float time = 0.0f, speed = 1.0f;
        float weight = 0.0f, target = 0.0f, fadeRate = 0.0f;
        bool playing = false, looped = false, stopping = false;
        AnimationPriority priority = AnimationPriority::Core;
    };
    Scene* server_ = nullptr;
    std::vector<Scene*> clients_;
    std::unordered_map<uint64_t, Track> tracks_;
    std::vector<uint64_t> order_;               // load order (stable blending)
    std::vector<AnimEvent> events_;
    std::unordered_map<std::string, std::shared_ptr<AnimClip>> files_;
    uint64_t nextId_ = 1;

    Track* find(uint64_t id);
    const Track* find(uint64_t id) const;
    void event(const Track& t, AnimEvent::Kind kind, const std::string& name = {}, const std::string& value = {});
    void crossings(const Track& t, float from, float to); // keyframe / marker events passed between two times
};

} // namespace rift
