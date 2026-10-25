// Animations: clips (.anim files, KeyframeSequences), easing, Motor6D joints,
// track blending, the Lua API and the default character animations.
#include "doctest.h"
#include "game/Animation.h"
#include "game/GameSession.h"
#include "platform/Input.h"
#include "scene/Reflection.h"
#include "scene/Scene.h"
#include "scene/SceneIO.h"
#include "script/ScriptStore.h"
#include "core/Paths.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    // The right arm swinging forward 90 degrees over one second.
    std::shared_ptr<AnimClip> swingClip(AnimationPriority priority = AnimationPriority::Action, bool loop = false) {
        nlohmann::json j = {
            { "name", "Swing" }, { "loop", loop }, { "priority", "Action" },
            { "keyframes", {
                { { "time", 0.0 }, { "poses", { { "Right Arm", { 0, 0, 0, 0, 0, 0 } } } } },
                { { "time", 0.5 }, { "name", "Half" }, { "poses", { { "Right Arm", { 0, 0, 0, 45, 0, 0 } } } } },
                { { "time", 1.0 }, { "poses", { { "Right Arm", { 0, 0, 0, 90, 0, 0 } } } } } } },
            { "markers", { { { "time", 0.75 }, { "name", "Hit" }, { "value", "boom" } } } },
        };
        auto clip = clipFromJson(j);
        REQUIRE(clip);
        clip->priority = priority;
        return clip;
    }

    float pitchOf(const glm::quat& q) {
        const glm::vec3 down = q * glm::vec3(0.0f, -1.0f, 0.0f);
        return glm::degrees(std::atan2(-down.z, -down.y)); // forward swing of a limb hanging down
    }
}

TEST_CASE("animation: clips read, sample, ease and write back") {
    auto clip = swingClip();
    CHECK(clip->length() == doctest::Approx(1.0f));
    std::unordered_map<std::string, AnimPose> poses;
    sampleClip(*clip, 0.25f, poses);
    REQUIRE(poses.count("Right Arm"));
    CHECK(pitchOf(poses["Right Arm"].rotation) == doctest::Approx(22.5f).epsilon(0.02));
    sampleClip(*clip, 5.0f, poses); // past the end: the last pose
    CHECK(pitchOf(poses["Right Arm"].rotation) == doctest::Approx(90.0f).epsilon(0.02));

    // Easing curves.
    CHECK(easePose(PoseEasingStyle::Linear, PoseEasingDirection::In, 0.3f) == doctest::Approx(0.3f));
    CHECK(easePose(PoseEasingStyle::Constant, PoseEasingDirection::In, 0.9f) == 0.0f);
    CHECK(easePose(PoseEasingStyle::Cubic, PoseEasingDirection::In, 0.5f) == doctest::Approx(0.125f));
    CHECK(easePose(PoseEasingStyle::Cubic, PoseEasingDirection::Out, 0.5f) == doctest::Approx(0.875f));
    CHECK(easePose(PoseEasingStyle::Cubic, PoseEasingDirection::InOut, 0.5f) == doctest::Approx(0.5f));
    CHECK(easePose(PoseEasingStyle::Bounce, PoseEasingDirection::Out, 1.0f) == doctest::Approx(1.0f));
    CHECK(easePose(PoseEasingStyle::Elastic, PoseEasingDirection::Out, 1.0f) == doctest::Approx(1.0f));

    // JSON round trip, and through a KeyframeSequence.
    auto again = clipFromJson(clipToJson(*clip));
    REQUIRE(again);
    CHECK(again->keyframes.size() == 3);
    CHECK(again->keyframes[1].name == "Half");
    REQUIRE(again->markers.size() == 1);
    CHECK(again->markers[0].value == "boom");
    Scene scene;
    Instance* kfs = scene.create(ClassId::KeyframeSequence, &scene.workspace());
    clipToSequence(*clip, scene, *kfs);
    auto fromSeq = clipFromSequence(*kfs);
    REQUIRE(fromSeq->keyframes.size() == 4); // + an empty keyframe holding the marker
    sampleClip(*fromSeq, 0.5f, poses);
    CHECK(pitchOf(poses["Right Arm"].rotation) == doctest::Approx(45.0f).epsilon(0.02));
    REQUIRE(fromSeq->markers.size() == 1);
    CHECK(fromSeq->markers[0].name == "Hit");

    // The engine's own animations all load.
    for (const char* name : { "idle", "walk", "run", "jump", "fall", "swim", "toolnone" }) {
        std::string error;
        auto c = loadClipFile(Paths::engineData() / "animations" / (std::string(name) + ".anim"), &error);
        CHECK_MESSAGE(c, name << ": " << error);
        if (c) CHECK(c->length() > 0.0f);
    }
}

TEST_CASE("animation: Motor6D joints place Part1 from Part0, C0, Transform and C1") {
    Scene scene;
    Instance* rig = buildCharacterRig(scene, RigColors{}, true);
    scene.setParent(rig, &scene.workspace());
    rig->position = { 10, 3, 0 };
    Instance* arm = rig->findFirstChild("Right Arm");
    REQUIRE(arm);
    const glm::vec3 rest = arm->worldPosition();
    auto joints = rigJoints(*rig);
    REQUIRE(joints.count("Right Arm"));
    REQUIRE(joints.count("Torso"));
    REQUIRE(joints.count("Head"));
    // At rest the joints keep the built layout.
    solveJoints(scene, *rig);
    CHECK(glm::length(arm->worldPosition() - rest) < 1e-3f);
    // Swing the shoulder forward 90 degrees: the arm sticks out in front (-Z) at shoulder height.
    joints["Right Arm"]->jointTransform.rotation = { 90, 0, 0 };
    solveJoints(scene, *rig);
    const glm::vec3 p = arm->worldPosition();
    CHECK(p.z < rest.z - 0.3f);
    CHECK(p.y > rest.y + 0.3f);
    CHECK(p.x == doctest::Approx(rest.x).epsilon(0.01));
    // The eyes ride on the head.
    Instance* head = rig->findFirstChild("Head");
    REQUIRE(head->findFirstChild("LeftEye"));
}

TEST_CASE("animation: tracks fade, loop, fire events and blend by priority") {
    Scene scene;
    Instance* rig = buildCharacterRig(scene, RigColors{}, true);
    scene.setParent(rig, &scene.workspace());
    Instance* hum = rig->findFirstChild("Humanoid");
    REQUIRE(hum);
    Instance* animator = hum->children.front();
    REQUIRE(animator->cls == ClassId::Animator);
    Instance* kfs = scene.create(ClassId::KeyframeSequence, &scene.workspace());
    clipToSequence(*swingClip(), scene, *kfs);
    kfs->priority = AnimationPriority::Action;
    Instance* anim = scene.create(ClassId::Animation, &scene.workspace());
    anim->animationId = "kfs:" + idToString(kfs->id);

    AnimationSystem system;
    system.setScenes(&scene, {});
    std::string error;
    const uint64_t track = system.load(-1, scene, *animator, *anim, error);
    REQUIRE_MESSAGE(track, error);
    CHECK(system.info(track).length == doctest::Approx(1.0f));
    CHECK_FALSE(system.info(track).playing);
    system.play(track, 0.0f);
    CHECK(system.info(track).playing);
    for (int k = 0; k < 30; ++k) system.update(1.0f / 60.0f);
    CHECK(system.info(track).timePosition == doctest::Approx(0.5f).epsilon(0.05));
    auto joints = rigJoints(*rig);
    CHECK(joints["Right Arm"]->jointTransform.rotation.x == doctest::Approx(45.0f).epsilon(0.05));

    // A lower priority track on the same joint does not show through.
    Instance* kfs2 = scene.create(ClassId::KeyframeSequence, &scene.workspace());
    auto back = swingClip();
    for (auto& k : back->keyframes) k.poses["Right Arm"].rotation = glm::quat(glm::radians(glm::vec3(-60, 0, 0)));
    back->prepare();
    back->loop = true;
    clipToSequence(*back, scene, *kfs2);
    kfs2->priority = AnimationPriority::Idle;
    Instance* anim2 = scene.create(ClassId::Animation, &scene.workspace());
    anim2->animationId = "kfs:" + idToString(kfs2->id);
    const uint64_t low = system.load(-1, scene, *animator, *anim2, error);
    system.play(low, 0.0f);
    system.update(1.0f / 60.0f);
    CHECK(joints["Right Arm"]->jointTransform.rotation.x > 40.0f);
    CHECK(system.playingTracks(animator->id).size() == 2);

    // Events: the named keyframe and the marker were passed, then the end.
    bool half = false, hit = false, stopped = false, ended = false;
    for (int k = 0; k < 90; ++k) {
        system.update(1.0f / 60.0f);
        for (const AnimEvent& e : system.takeEvents()) {
            if (e.track != track) continue;
            half |= e.kind == AnimEvent::KeyframeReached && e.name == "Half";
            hit |= e.kind == AnimEvent::Marker && e.name == "Hit" && e.value == "boom";
            stopped |= e.kind == AnimEvent::Stopped;
            ended |= e.kind == AnimEvent::Ended;
        }
    }
    CHECK(hit);
    CHECK(stopped);
    CHECK(ended);
    CHECK_FALSE(system.info(track).playing);
    // Now only the low priority track moves the arm.
    CHECK(joints["Right Arm"]->jointTransform.rotation.x < -50.0f);
    (void)half;

    // Looping: DidLoop fires and time wraps.
    system.setLooped(track, true);
    system.play(track, 0.0f);
    bool looped = false;
    for (int k = 0; k < 80; ++k) {
        system.update(1.0f / 60.0f);
        for (const AnimEvent& e : system.takeEvents()) looped |= e.track == track && e.kind == AnimEvent::DidLoop;
    }
    CHECK(looped);
    CHECK(system.info(track).timePosition < 0.5f);

    // The Animator gone: its tracks go too.
    scene.destroy(rig);
    system.update(1.0f / 60.0f);
    CHECK_FALSE(system.exists(track));
}

namespace {
    struct AnimGame {
        Scene scene;
        ScriptStore store;
        Input input;
        fs::path folder;
        std::unique_ptr<GameSession> session;
        AnimGame() {
            folder = fs::temp_directory_path() / ("rift_anim_game_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(folder);
            fs::create_directories(folder);
            store.reset(Paths::toUtf8(folder));
            Instance* base = scene.create(ClassId::Part, &scene.workspace());
            base->size = { 200, 1, 200 };
            base->position = { 0, -0.5f, 0 };
        }
        ~AnimGame() {
            session.reset();
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
        void start() {
            session = std::make_unique<GameSession>(scene, store, input, GameSession::Options{});
            session->start();
        }
        void run(float seconds) {
            ViewportInput view;
            view.keyboard = view.hovered = true;
            view.size = { 800, 600 };
            for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) {
                input.newFrame();
                session->update(1.0f / 60.0f, view);
            }
        }
        bool printed(const std::string& text) const {
            for (const auto& line : session->logLines()) if (line.text == text) return true;
            return false;
        }
        bool anyError() const {
            for (const auto& line : session->logLines()) if (line.level == LogLevel::Error) return true;
            return false;
        }
        std::string dump() const {
            std::string out;
            for (const auto& line : session->logLines()) out += line.text + "\n";
            return out;
        }
    };
}

TEST_CASE("animation: the default character idles, walks and holds tools with animations") {
    AnimGame g;
    g.start();
    g.run(1.0f);
    REQUIRE_FALSE_MESSAGE(g.anyError(), g.dump());
    Instance* model = g.session->character();
    REQUIRE(model);
    REQUIRE(model->findFirstChild("Animate"));
    Instance* leg = model->findFirstChild("Right Leg");
    REQUIRE(leg);
    // Walking forward swings the legs.
    g.input.setKey(GLFW_KEY_W, true);
    float lowest = 1e9f, highest = -1e9f;
    for (int k = 0; k < 90; ++k) {
        g.run(1.0f / 60.0f);
        const float x = leg->rotation.x;
        lowest = std::min(lowest, x);
        highest = std::max(highest, x);
    }
    g.input.setKey(GLFW_KEY_W, false);
    CHECK(highest - lowest > 20.0f);
    // The client sees the same limbs (replicated from the server's world).
    const Instance* clientLeg = g.session->clientScene().find(leg->id);
    REQUIRE(clientLeg);
    CHECK(glm::length(clientLeg->worldPosition() - leg->worldPosition()) < 0.05f);
    // A tool raises the right arm.
    g.run(1.0f);
    Instance* arm = model->findFirstChild("Right Arm");
    const float armBefore = arm->rotation.x;
    Instance* tool = g.scene.create(ClassId::Tool);
    (void)tool;
    Instance* liveTool = g.session->scene().create(ClassId::Tool);
    liveTool->requiresHandle = false;
    g.session->scene().setParent(liveTool, model);
    g.run(0.6f);
    CHECK(arm->rotation.x > armBefore + 60.0f);
    CHECK_FALSE_MESSAGE(g.anyError(), g.dump());
}

TEST_CASE("animation: the Lua API (LoadAnimation, tracks, events, states)") {
    AnimGame g;
    // An NPC to animate, built like the default character.
    Instance* npc = buildCharacterRig(g.scene, RigColors{}, true);
    npc->name = "Dummy";
    npc->position = { 10, 1, 0 };
    g.scene.setParent(npc, &g.scene.workspace());
    g.store.create("scripts/Anim.lua", R"(
        local dummy = workspace.Dummy
        local kfs = Instance.new("KeyframeSequence")
        kfs.Priority = Enum.AnimationPriority.Action
        local function key(t, angle, name)
            local k = Instance.new("Keyframe")
            k.Time = t
            if name then k.Name = name end
            local p = Instance.new("Pose")
            p.Name = "Right Arm"
            p.CFrame = CFrame.Angles(math.rad(angle), 0, 0)
            p.Parent = k
            k.Parent = kfs
            return k
        end
        key(0, 0)
        local mid = key(0.5, 45, "Mid")
        local marker = Instance.new("KeyframeMarker")
        marker.Name = "Clap"
        marker.Value = "now"
        marker.Parent = mid
        key(1, 90)
        kfs.Parent = workspace
        local anim = Instance.new("Animation")
        anim.AnimationId = game:GetService("KeyframeSequenceProvider"):RegisterKeyframeSequence(kfs)
        local track = dummy.Humanoid:LoadAnimation(anim)
        print("loaded", track.Length, track.IsPlaying, track.Priority == Enum.AnimationPriority.Action)
        track.KeyframeReached:Connect(function(name) print("keyframe", name) end)
        track:GetMarkerReachedSignal("Clap"):Connect(function(value) print("marker", value) end)
        track.Stopped:Connect(function() print("stopped") end)
        track.Ended:Connect(function() print("ended") end)
        dummy.Humanoid.Animator.AnimationPlayed:Connect(function(t) print("played", t == track) end)
        track:Play(0)
        print("playing", track.IsPlaying, #dummy.Humanoid.Animator:GetPlayingAnimationTracks())
        task.wait(0.6)
        print("time", track.TimePosition > 0.5 and track.TimePosition < 0.75)
        print("keyframe time", track:GetTimeOfKeyframe("Mid"))
        print("joint", math.floor(math.deg(select(1, dummy.Torso["Right Shoulder"].Transform:ToEulerAnglesXYZ())) + 0.5) > 40)
        local ok = pcall(function() dummy.Torso["Right Shoulder"].Part0 = Instance.new("Folder") end)
        print("part0 refused", not ok)
        local bad = Instance.new("Animation")
        print("bad id", (pcall(function() dummy.Humanoid:LoadAnimation(bad) end)))
    )");
    Instance* s = g.scene.create(ClassId::Script, g.scene.service(ClassId::ScriptService));
    s->source = "scripts/Anim.lua";
    g.store.create("scripts/State.lua", R"(
        local player = game.Players.LocalPlayer
        local character = player.Character or player.CharacterAdded:Wait()
        local humanoid = character:WaitForChild("Humanoid")
        humanoid.StateChanged:Connect(function(old, new) print("state", new.Name) end)
        humanoid.FreeFalling:Connect(function(active) if active then print("falling") end end)
        print("first state", humanoid:GetState().Name)
    )");
    Instance* local = g.scene.create(ClassId::LocalScript);
    local->source = "scripts/State.lua";
    for (Instance* c : g.scene.starterPlayer().children)
        if (c->cls == ClassId::StarterCharacterScripts) g.scene.setParent(local, c);
    g.start();
    g.run(1.8f);
    CHECK_MESSAGE(g.printed("loaded 1 false true"), g.dump());
    CHECK(g.printed("played true"));
    CHECK(g.printed("playing true 1"));
    CHECK(g.printed("keyframe Mid"));
    CHECK(g.printed("marker now"));
    CHECK(g.printed("time true"));
    CHECK(g.printed("keyframe time 0.5"));
    CHECK(g.printed("joint true"));
    CHECK(g.printed("part0 refused true"));
    CHECK(g.printed("bad id false"));
    CHECK(g.printed("stopped"));
    CHECK(g.printed("ended"));
    CHECK(g.printed("first state Running"));
    // Jumping and landing go through Jumping and Freefall.
    g.input.setKey(GLFW_KEY_SPACE, true);
    g.run(0.1f);
    g.input.setKey(GLFW_KEY_SPACE, false);
    g.run(1.5f);
    CHECK_MESSAGE(g.printed("state Jumping"), g.dump());
    CHECK(g.printed("state Freefall"));
    CHECK(g.printed("falling"));
    CHECK(g.printed("state Running"));
}
