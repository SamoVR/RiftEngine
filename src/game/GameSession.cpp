#include "game/GameSession.h"
#include "physics/PhysicsWorld.h"
#include "script/LuaRuntime.h"
#include "script/ScriptStore.h"
#include "audio/AudioSystem.h"
#include "platform/Input.h"
#include "core/Paths.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace rift {

namespace {
    constexpr float kCharacterHeight = 2.0f;
    constexpr float kCharacterRadius = 0.45f;
    constexpr float kRespawnDelay = 3.0f;
    constexpr size_t kMaxLogLines = 2000;

    Instance* addPart(Scene& scene, Instance* parent, const char* name, PartShape shape, glm::vec3 pos, glm::vec3 size, Color3 color) {
        Instance* p = scene.create(ClassId::Part, parent);
        p->name = name;
        p->shape = shape;
        p->position = pos;
        p->size = size;
        p->color = color;
        p->material = Material::SmoothPlastic;
        p->anchored = false;
        p->canCollide = false;
        return p;
    }

    float wrapDegrees(float a) {
        a = std::fmod(a + 180.0f, 360.0f);
        if (a < 0.0f) a += 360.0f;
        return a - 180.0f;
    }
}

// ============================================================================
//  Lifetime
// ============================================================================
GameSession::GameSession(const Scene& editScene, ScriptStore& scripts, Input& input, Options options)
    : scene_(editScene.clone()), scripts_(scripts), input_(input), options_(std::move(options)), gui_(*scene_) {
    physics_ = std::make_unique<PhysicsWorld>(*scene_);
    lua_ = std::make_unique<LuaRuntime>(*this);
    gui_.onEvent = [this](Instance& element, GuiEvent e) { lua_->onGuiEvent(element, e); };

    Instance* player = scene_->create(ClassId::Player, scene_->service(ClassId::Players));
    player->name = options_.playerName;
    playerId_ = player->id;

    // Start where the editor camera was looking from is the host's business;
    // by default look at the world from a little above.
    camera_.lookAt({ 0.0f, 12.0f, 24.0f }, { 0.0f, 0.0f, 0.0f });
}

GameSession::~GameSession() {
    if (options_.audio) {
        for (auto& [id, s] : sounds_) options_.audio->stop(s.handle);
    }
    sounds_.clear();
    lua_.reset();     // observes the scene; goes first
    physics_.reset();
}

void GameSession::start() {
    if (started_) return;
    started_ = true;
    if (options_.spawnCharacter) spawnCharacter();
    lua_->start();
    gui_.layout(view_.size);
}

// ============================================================================
//  Frame
// ============================================================================
void GameSession::update(float dt, const ViewportInput& view) {
    view_ = view;
    if (!started_) return;
    if (messageTimer_ > 0.0f) {
        messageTimer_ -= dt;
        if (messageTimer_ <= 0.0f && outcome_ == Outcome::None) message_.clear();
    }
    gui_.layout(view.size);
    if (paused_ || outcome_ != Outcome::None) {
        updateCamera(0.0f);
        return;
    }
    time_ += dt;

    gui_.handleMouse(view.mouse, view.hovered, view.hovered && input_.mouseDown(GLFW_MOUSE_BUTTON_LEFT));
    lua_->beforePhysics(dt);
    updateCharacter(dt);
    physics_->step(dt);
    afterPhysics(dt);
    lua_->afterPhysics(dt);
    updateCamera(dt);
    updateSounds();
    gui_.layout(view.size); // scripts may have changed the GUI this frame
}

// ============================================================================
//  Player / character
// ============================================================================
Instance* GameSession::player() const { return scene_->find(playerId_); }
Instance* GameSession::character() const { return characterId_ ? scene_->find(characterId_) : nullptr; }
Instance* GameSession::humanoid() const { return humanoidId_ ? scene_->find(humanoidId_) : nullptr; }
Instance* GameSession::rootPart() const { return rootPartId_ ? scene_->find(rootPartId_) : nullptr; }

glm::vec3 GameSession::spawnPosition() const {
    std::vector<Instance*> spawns;
    Scene::forEachDescendant(scene_->workspace(), [&](Instance& i) {
        if (i.cls == ClassId::SpawnLocation && i.enabled) spawns.push_back(&i);
    });
    if (spawns.empty()) return { 0.0f, 3.0f, 0.0f };
    const Instance* s = spawns[static_cast<size_t>(time_ * 1000.0) % spawns.size()];
    const glm::vec3 top = glm::vec3(s->worldTransform() * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
    return top + glm::vec3(0.0f, 0.05f, 0.0f);
}

void GameSession::spawnCharacter() {
    destroyCharacter();
    for (uint64_t id : deathPieces_) if (Instance* piece = scene_->find(id)) scene_->destroy(piece);
    deathPieces_.clear();
    Instance* playerInst = player();
    Instance& starter = scene_->starterPlayer();
    Instance* model = nullptr;

    // A custom character: StarterPlayer.StarterCharacter (a Model with a HumanoidRootPart and a Humanoid).
    if (Instance* custom = starter.findFirstChild("StarterCharacter"); custom && custom->cls == ClassId::Model) {
        Instance* root = custom->findFirstChild("HumanoidRootPart");
        bool hasHumanoid = false;
        for (Instance* c : custom->children) hasHumanoid |= c->cls == ClassId::Humanoid;
        if (root && root->isBasePart() && hasHumanoid) model = scene_->cloneSubtree(*custom);
        else log(LogLevel::Warning, "StarterCharacter needs a HumanoidRootPart and a Humanoid; using the default character");
    }
    if (!model) {
        model = scene_->create(ClassId::Model);
        const Color3 shirt = starter.color;
        const Color3 skin(0.98f, 0.82f, 0.55f);
        const Color3 pants(0.17f, 0.20f, 0.28f);
        Instance* root = addPart(*scene_, model, "HumanoidRootPart", PartShape::Block, { 0, 0, 0 }, { 1.0f, kCharacterHeight, 1.0f }, shirt);
        root->transparency = 1.0f;
        root->castShadow = false;
        addPart(*scene_, model, "Torso", PartShape::Block, { 0, 0.15f, 0 }, { 1.0f, 0.8f, 0.55f }, shirt);
        addPart(*scene_, model, "Head", PartShape::Ball, { 0, 0.8f, 0 }, { 0.55f, 0.55f, 0.55f }, skin);
        addPart(*scene_, model, "Left Arm", PartShape::Block, { -0.66f, 0.12f, 0 }, { 0.3f, 0.75f, 0.32f }, skin);
        addPart(*scene_, model, "Right Arm", PartShape::Block, { 0.66f, 0.12f, 0 }, { 0.3f, 0.75f, 0.32f }, skin);
        addPart(*scene_, model, "Left Leg", PartShape::Block, { -0.24f, -0.625f, 0 }, { 0.42f, 0.75f, 0.45f }, pants);
        addPart(*scene_, model, "Right Leg", PartShape::Block, { 0.24f, -0.625f, 0 }, { 0.42f, 0.75f, 0.45f }, pants);
        Instance* eyeL = addPart(*scene_, model, "LeftEye", PartShape::Ball, { -0.1f, 0.86f, -0.24f }, { 0.08f, 0.1f, 0.06f }, { 0.08f, 0.08f, 0.1f });
        Instance* eyeR = addPart(*scene_, model, "RightEye", PartShape::Ball, { 0.1f, 0.86f, -0.24f }, { 0.08f, 0.1f, 0.06f }, { 0.08f, 0.08f, 0.1f });
        eyeL->castShadow = eyeR->castShadow = false;
        scene_->create(ClassId::Humanoid, model);
    }
    model->name = playerInst ? playerInst->name : "Player";

    Instance* root = model->findFirstChild("HumanoidRootPart");
    Instance* hum = nullptr;
    for (Instance* c : model->children) if (c->cls == ClassId::Humanoid) hum = c;
    hum->walkSpeed = starter.walkSpeed;
    hum->jumpPower = starter.jumpPower;
    hum->health = hum->maxHealth;

    // The model's frame is the root part's centre; the parts hang off it.
    const glm::vec3 feet = spawnPosition();
    model->position = feet + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f);
    model->rotation = glm::vec3(0.0f, facingYaw_, 0.0f);
    if (root->position != glm::vec3(0.0f)) {
        // Custom characters: whatever layout they have, the root is the reference.
        model->position -= root->position;
    }
    physics_->setIgnored(model->id, true); // the controller moves it, not rigid-body physics
    scene_->setParent(model, &scene_->workspace());

    characterId_ = model->id;
    humanoidId_ = hum->id;
    rootPartId_ = root->id;
    controller_ = physics_->createCharacter(root->id, feet, kCharacterRadius, kCharacterHeight);
    lastHealth_ = hum->health;
    dead_ = false;
    walkTarget_ = false;
    runningSpeed_ = -1.0f;

    // Camera starts behind the new character.
    camera_.yaw = facingYaw_;
    if (started_) lua_->onCharacterAdded(*model);
}

void GameSession::destroyCharacter() {
    if (controller_) {
        physics_->destroyCharacter(controller_);
        controller_ = nullptr;
    }
    if (Instance* c = character()) {
        lua_->onCharacterRemoving(*c);
        scene_->destroy(c);
    }
    characterId_ = humanoidId_ = rootPartId_ = 0;
}

void GameSession::loadCharacter() { spawnCharacter(); }

void GameSession::moveCharacterTo(const glm::vec3& target) {
    walkTarget_ = true;
    walkTo_ = target;
}

void GameSession::updateCharacter(float dt) {
    (void)dt;
    if (!controller_) {
        if (dead_ && options_.spawnCharacter) {
            respawnTimer_ -= dt;
            if (respawnTimer_ <= 0.0f) spawnCharacter();
        }
        return;
    }
    Instance* model = character();
    Instance* hum = humanoid();
    Instance* root = rootPart();
    if (!model || !hum || !root) { // a script destroyed (part of) the character
        if (controller_) { physics_->destroyCharacter(controller_); controller_ = nullptr; }
        if (Instance* m = character()) scene_->destroy(m);
        characterId_ = humanoidId_ = rootPartId_ = 0;
        dead_ = true;
        respawnTimer_ = kRespawnDelay;
        return;
    }

    // A script moved the character (CFrame / PivotTo / Position): teleport the capsule.
    const glm::vec3 expected = controller_->feetPosition() + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f);
    const glm::vec3 actual = root->worldPosition();
    if (glm::length(actual - expected) > 0.01f) controller_->teleport(actual - glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f));
    // A script set the root part's velocity (jump pads, knock-back).
    if (glm::length(root->velocity - controller_->velocity()) > 0.01f) controller_->setVelocity(root->velocity);

    CharacterController::Input in;
    in.walkSpeed = hum->walkSpeed;
    in.jumpSpeed = hum->jumpPower;
    if (!dead_) {
        glm::vec3 wish(0.0f);
        if (view_.keyboard) {
            glm::vec3 forward = camera_.lookVector();
            forward.y = 0.0f;
            forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            if (input_.keyDown(GLFW_KEY_W) || input_.keyDown(GLFW_KEY_UP)) wish += forward;
            if (input_.keyDown(GLFW_KEY_S) || input_.keyDown(GLFW_KEY_DOWN)) wish -= forward;
            if (input_.keyDown(GLFW_KEY_D) || input_.keyDown(GLFW_KEY_RIGHT)) wish += right;
            if (input_.keyDown(GLFW_KEY_A) || input_.keyDown(GLFW_KEY_LEFT)) wish -= right;
            if (input_.keyDown(GLFW_KEY_SPACE)) in.jump = true;
        }
        if (glm::length(wish) > 1e-4f) {
            wish = glm::normalize(wish);
            walkTarget_ = false;
        } else if (walkTarget_) {
            glm::vec3 to = walkTo_ - controller_->feetPosition();
            to.y = 0.0f;
            if (glm::length(to) < 0.3f) walkTarget_ = false;
            else wish = glm::normalize(to);
        }
        if (hum->jump) { in.jump = true; hum->jump = false; }
        in.moveDirection = wish;
    }
    hum->moveDirection = in.moveDirection;
    controller_->setInput(in);
}

void GameSession::afterPhysics(float dt) {
    for (const TouchEvent& e : physics_->takeTouchEvents()) lua_->onTouch(e);
    for (uint64_t id : physics_->takeFallenParts()) {
        if (Instance* i = scene_->find(id)) scene_->destroy(i); // fell out of the world
    }

    Instance* model = character();
    Instance* hum = humanoid();
    Instance* root = rootPart();
    if (!controller_ || !model || !hum || !root) return;

    // Place the model on the capsule and turn it towards where it walks.
    const glm::vec3 feet = controller_->feetPosition();
    const glm::vec3 move = hum->moveDirection;
    const bool firstPerson = scene_->starterPlayer().cameraMode == CameraMode::FirstPerson || camera_.zoom < 1.0f;
    float targetYaw = facingYaw_;
    if (firstPerson) targetYaw = camera_.yaw;
    else if (glm::length(move) > 0.1f) targetYaw = glm::degrees(std::atan2(-move.x, -move.z));
    const float turn = wrapDegrees(targetYaw - facingYaw_);
    facingYaw_ = wrapDegrees(facingYaw_ + turn * std::min(1.0f, dt * 12.0f));
    const glm::vec3 rootOffset = root->position; // custom characters may have an offset root
    model->setWorldFrame(feet + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f) - rootOffset, glm::vec3(0.0f, facingYaw_, 0.0f));
    root->velocity = controller_->velocity();

    if (controller_->justJumped()) lua_->onJumping(*hum);
    const float speed = glm::length(glm::vec2(root->velocity.x, root->velocity.z));
    if (std::abs(speed - runningSpeed_) > 0.5f || (speed == 0.0f && runningSpeed_ != 0.0f)) {
        runningSpeed_ = speed < 0.05f ? 0.0f : speed;
        lua_->onRunning(*hum, runningSpeed_);
    }

    // Health and death.
    if (feet.y < scene_->workspace().fallenPartsHeight && hum->health > 0.0f) scene_->setProperty(*hum, "Health", 0.0f);
    if (hum->health > hum->maxHealth) hum->health = hum->maxHealth;
    if (hum->health != lastHealth_) {
        lastHealth_ = hum->health;
        lua_->onHealthChanged(*hum, hum->health);
    }
    if (hum->health <= 0.0f && !dead_) {
        dead_ = true;
        respawnTimer_ = kRespawnDelay;
        lua_->onHumanoidDied(*hum);
        controller_->setInput({});
        // Fall apart: the limbs drop (they become ordinary unanchored parts).
        physics_->destroyCharacter(controller_);
        controller_ = nullptr;
        physics_->setIgnored(model->id, false);
        for (Instance* c : model->children) {
            if (!c->isBasePart()) continue;
            c->canCollide = c->name != "HumanoidRootPart";
            c->velocity = glm::vec3(std::sin(c->position.x * 7.0f), 3.0f, std::cos(c->position.x * 5.0f)) * 2.0f;
        }
        // Detach the parts so each falls on its own; they are removed when
        // the next character spawns.
        for (Instance* c : std::vector<Instance*>(model->children)) {
            if (!c->isBasePart() || c->name == "HumanoidRootPart") continue;
            const glm::mat4 world = c->worldFrame();
            scene_->setParent(c, &scene_->workspace());
            c->setWorldTransform(world * glm::scale(glm::mat4(1.0f), c->size));
            c->archivable = false;
            deathPieces_.push_back(c->id);
        }
    }
}

// ============================================================================
//  Camera
// ============================================================================
void GameSession::updateCamera(float dt) {
    (void)dt;
    const bool firstPersonMode = scene_->starterPlayer().cameraMode == CameraMode::FirstPerson;
    const float maxZoom = std::max(1.0f, scene_->starterPlayer().cameraMaxZoom);
    if (firstPersonMode) camera_.zoom = 0.5f;
    const bool firstPerson = camera_.zoom < 1.0f;
    const bool rotating = view_.hovered && input_.mouseDown(GLFW_MOUSE_BUTTON_RIGHT);
    mouseLock_ = (firstPerson && view_.hovered) || scriptMouseLock_;

    if (camera_.type == CameraType::Custom) {
        if (rotating || mouseLock_) {
            const glm::vec2 d = input_.mouseDelta();
            camera_.yaw = wrapDegrees(camera_.yaw - d.x * 0.3f);
            camera_.pitch = std::clamp(camera_.pitch - d.y * 0.3f, -80.0f, 80.0f);
        }
        if (view_.hovered && input_.scroll() != 0.0f && !firstPersonMode) {
            camera_.zoom = std::clamp(camera_.zoom * (1.0f - input_.scroll() * 0.12f), 0.5f, maxZoom);
            if (camera_.zoom < 1.0f && input_.scroll() < 0.0f) camera_.zoom = 1.0f; // zoom back out of first person
        }
        const Instance* root = rootPart();
        glm::vec3 focus = root ? root->worldPosition() + glm::vec3(0.0f, 0.8f, 0.0f) : camera_.position;
        if (!root && dead_) focus = camera_.position + camera_.lookVector() * camera_.zoom;
        const float yaw = glm::radians(camera_.yaw), pitch = glm::radians(camera_.pitch);
        const glm::vec3 forward(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
        float distance = firstPerson ? 0.0f : camera_.zoom;
        if (distance > 0.0f && root) {
            // Walls between the character and the camera pull it closer.
            RaycastFilter filter;
            filter.respectCanCollide = true;
            if (Instance* c = character()) filter.instances.push_back(c->id);
            if (auto hit = physics_->raycast(focus, -forward * (distance + 0.3f), filter)) distance = std::max(0.3f, hit->distance - 0.3f);
        }
        camera_.position = focus - forward * distance;
        camera_.lookAt(camera_.position, camera_.position + forward);
    }
    if (options_.audio) options_.audio->setListener(camera_.position, camera_.lookVector(), camera_.upVector());
}

bool GameSession::screenPointToRay(const glm::vec2& point, glm::vec3& origin, glm::vec3& direction) const {
    if (view_.size.x <= 0.0f || view_.size.y <= 0.0f) return false;
    const glm::vec2 ndc(2.0f * point.x / view_.size.x - 1.0f, 1.0f - 2.0f * point.y / view_.size.y);
    const glm::mat4 inv = glm::inverse(camera_.projection(view_.size.x / view_.size.y) * camera_.view());
    glm::vec4 nearP = inv * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 farP = inv * glm::vec4(ndc, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    origin = glm::vec3(nearP);
    direction = glm::normalize(glm::vec3(farP - nearP));
    return true;
}

glm::vec3 GameSession::worldToScreen(const glm::vec3& world) const {
    const glm::vec4 clip = camera_.projection(view_.size.x / std::max(1.0f, view_.size.y)) * camera_.view() * glm::vec4(world, 1.0f);
    if (std::abs(clip.w) < 1e-6f) return { 0.0f, 0.0f, 0.0f };
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return { (ndc.x + 1.0f) * 0.5f * view_.size.x, (1.0f - ndc.y) * 0.5f * view_.size.y, clip.w };
}

// ============================================================================
//  Output
// ============================================================================
void GameSession::log(LogLevel level, const std::string& text, uint64_t script) {
    log_.push_back({ text, level, time_, script });
    ++logCounter_;
    while (log_.size() > kMaxLogLines) log_.pop_front();
    if (level != LogLevel::Info) Log::write(level, "Script", text);
}

void GameSession::showMessage(const std::string& text, float seconds) {
    message_ = text;
    messageTimer_ = seconds;
}

void GameSession::finish(bool won, const std::string& text) {
    if (outcome_ != Outcome::None) return;
    outcome_ = won ? Outcome::Won : Outcome::Lost;
    message_ = text.empty() ? (won ? "You win!" : "Game over") : text;
    messageTimer_ = 0.0f;
    log(LogLevel::Info, message_);
}

// ============================================================================
//  Sounds: the Playing property drives playback (scripts call :Play() /
//  :Stop(), which set it; Playing = true in the editor means autoplay).
// ============================================================================
void GameSession::playSound(Instance& sound) {
    scene_->setProperty(sound, "Playing", true);
    auto it = sounds_.find(sound.id);
    if (it != sounds_.end() && options_.audio) {
        options_.audio->stop(it->second.handle); // :Play() restarts from the beginning
        sounds_.erase(it);
    }
    updateSounds();
}

void GameSession::stopSound(Instance& sound) {
    scene_->setProperty(sound, "Playing", false);
    updateSounds();
}

void GameSession::pauseSound(Instance& sound, bool paused) {
    auto it = sounds_.find(sound.id);
    if (it == sounds_.end()) {
        if (!paused) playSound(sound);
        return;
    }
    it->second.paused = paused;
    if (options_.audio) options_.audio->setPaused(it->second.handle, paused);
}

bool GameSession::soundPlaying(const Instance& sound) const {
    auto it = sounds_.find(sound.id);
    return it != sounds_.end() && !it->second.paused && options_.audio && options_.audio->isPlaying(it->second.handle);
}

float GameSession::soundTime(const Instance& sound) const {
    auto it = sounds_.find(sound.id);
    return it != sounds_.end() && options_.audio ? options_.audio->timePosition(it->second.handle) : 0.0f;
}

float GameSession::soundLength(const Instance& sound) const {
    auto it = sounds_.find(sound.id);
    return it != sounds_.end() && options_.audio ? options_.audio->length(it->second.handle) : 0.0f;
}

void GameSession::updateSounds() {
    AudioSystem* audio = options_.audio;
    const Instance* soundService = scene_->service(ClassId::SoundService);
    const float master = soundService ? soundService->volume : 1.0f;
    if (audio) audio->setMasterVolume(master);

    // Start sounds whose Playing is on.
    std::vector<Instance*> wanted;
    scene_->forEachInTree([&](Instance& i) { if (i.cls == ClassId::Sound && i.playing) wanted.push_back(&i); });
    for (Instance* s : wanted) {
        if (sounds_.count(s->id)) continue;
        SoundState state;
        if (audio && !s->soundId.path.empty()) {
            AudioSystem::PlayParams params;
            params.volume = s->volume;
            params.speed = s->playbackSpeed;
            params.looped = s->looped;
            params.rollOffDistance = s->rollOffDistance;
            if (s->parent && s->parent->isBasePart()) params.position = s->parent->worldPosition();
            state.handle = audio->play(Paths::resolveAsset(s->soundId.path), params);
            if (!state.handle) log(LogLevel::Warning, "Could not play " + s->soundId.path + " (" + s->fullName() + ")");
        }
        sounds_[s->id] = state;
    }
    // Update / stop / finish.
    for (auto it = sounds_.begin(); it != sounds_.end();) {
        Instance* s = scene_->find(it->first);
        if (!s || !s->playing) {
            if (audio) audio->stop(it->second.handle);
            it = sounds_.erase(it);
            continue;
        }
        if (audio && it->second.handle) {
            audio->setVolume(it->second.handle, s->volume);
            audio->setSpeed(it->second.handle, s->playbackSpeed);
            audio->setLooped(it->second.handle, s->looped);
            if (s->parent && s->parent->isBasePart()) audio->setPosition(it->second.handle, s->parent->worldPosition());
            if (!it->second.paused && !audio->isPlaying(it->second.handle)) {
                audio->stop(it->second.handle); // release the voice
                s->playing = false;             // ended
                lua_->onSoundEnded(*s);
                it = sounds_.erase(it);
                continue;
            }
        } else if (!audio || !it->second.handle) {
            // No audio device (or no file): the sound "ends" at once, so scripts waiting on Ended continue.
            s->playing = false;
            lua_->onSoundEnded(*s);
            it = sounds_.erase(it);
            continue;
        }
        ++it;
    }
}

} // namespace rift
