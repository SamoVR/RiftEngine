#include "game/GameSession.h"
#include "game/Replicator.h"
#include "physics/PhysicsWorld.h"
#include "script/LuaRuntime.h"
#include "script/ScriptStore.h"
#include "audio/AudioSystem.h"
#include "platform/Input.h"
#include "core/Paths.h"
#include "core/TransformUtil.h"

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
    constexpr int kMaxPlayers = 8;
    constexpr int kHotbarSlots = 10;

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

    Instance* handleOf(const Instance& tool) {
        Instance* h = tool.findFirstChild("Handle");
        return h && h->isBasePart() ? h : nullptr;
    }

    void setWorldMatrix(Instance& part, const glm::mat4& world) {
        glm::vec3 p, r, s;
        TransformUtil::decompose(world, p, r, s);
        part.setWorldFrame(p, r);
    }
}

// ============================================================================
//  Lifetime
// ============================================================================
GameSession::Client::Client() = default;
GameSession::Client::~Client() = default;

template <class F> void GameSession::forAll(F&& f) {
    f(*lua_);
    for (auto& c : clients_) f(*c->lua);
}

GameSession::GameSession(const Scene& editScene, ScriptStore& scripts, Input& input, Options options)
    : scene_(editScene.clone()), scripts_(scripts), input_(input), options_(std::move(options)) {
    scene_->ensureDefaults();
    physics_ = std::make_unique<PhysicsWorld>(*scene_);

    // The players (on the server first, so every client sees all of them).
    const int count = std::clamp(options_.playerCount, 1, kMaxPlayers);
    for (int i = 0; i < count; ++i) addClient(i);

    // Each client starts as a copy of the server's world.
    for (auto& c : clients_) {
        c->scene = std::make_unique<Scene>();
        c->replicator = std::make_unique<Replicator>(*scene_, *c->scene);
        c->replicator->start();
        setUpClientPlayer(*c);
    }

    lua_ = std::make_unique<LuaRuntime>(*this, NetworkSide::Server, 0);
    for (auto& c : clients_) {
        Client* client = c.get();
        client->lua = std::make_unique<LuaRuntime>(*this, NetworkSide::Client, client->index);
        client->gui = std::make_unique<GuiSystem>(*client->scene);
        client->gui->setRoot(client->playerGuiId);
        client->gui->onEvent = [client](Instance& element, GuiEvent e) { client->lua->onGuiEvent(element, e); };
        // Where the camera starts is the host's business; by default look at
        // the world from a little above.
        client->camera.lookAt({ 0.0f, 12.0f, 24.0f }, { 0.0f, 0.0f, 0.0f });
    }
}

GameSession::~GameSession() {
    stopAllSounds();
    for (auto& c : clients_) c->gui.reset();
    for (auto& c : clients_) c->lua.reset(); // the runtimes observe the scenes; they go first
    lua_.reset();
    for (auto& c : clients_) c->replicator.reset();
    physics_.reset();
}

void GameSession::addClient(int index) {
    auto c = std::make_unique<Client>();
    c->index = index;
    Instance* player = scene_->create(ClassId::Player, scene_->service(ClassId::Players));
    player->name = options_.playerCount > 1 ? options_.playerName + std::to_string(index + 1) : options_.playerName;
    c->playerId = player->id;
    Instance* playerGui = scene_->create(ClassId::PlayerGui, player);
    playerGui->name = "PlayerGui";
    c->playerGuiId = playerGui->id;
    Instance* backpack = scene_->create(ClassId::Backpack, player);
    backpack->name = "Backpack";
    c->backpackId = backpack->id;
    clients_.push_back(std::move(c));
}

void GameSession::setUpClientPlayer(Client& c) {
    // PlayerScripts exists only on the client: copies of StarterPlayerScripts.
    Scene& scene = *c.scene;
    Instance* player = scene.find(c.playerId);
    if (!player) return;
    Instance* scriptsFolder = scene.create(ClassId::PlayerScripts, player);
    scriptsFolder->name = "PlayerScripts";
    for (Instance* folder : std::vector<Instance*>(scene.starterPlayer().children)) {
        if (folder->cls != ClassId::StarterPlayerScripts) continue;
        for (Instance* item : std::vector<Instance*>(folder->children)) {
            if (Instance* copy = scene.cloneSubtree(*item)) scene.setParent(copy, scriptsFolder);
        }
    }
}

void GameSession::fillPlayerGui(Client& c, bool respawn) {
    // StarterGui is copied into PlayerGui (on the server, so it replicates).
    // On respawn, ScreenGuis with ResetOnSpawn off keep their old copy.
    Instance* gui = scene_->find(c.playerGuiId);
    Instance* starter = scene_->service(ClassId::StarterGui);
    if (!gui || !starter) return;
    for (Instance* item : std::vector<Instance*>(gui->children)) {
        const bool keep = respawn && item->cls == ClassId::ScreenGui && !item->resetOnSpawn;
        if (!keep) scene_->destroy(item);
    }
    for (Instance* item : std::vector<Instance*>(starter->children)) {
        if (respawn && item->cls == ClassId::ScreenGui && !item->resetOnSpawn) continue;
        if (Instance* copy = scene_->cloneSubtree(*item)) scene_->setParent(copy, gui);
    }
}

void GameSession::fillBackpack(Client& c) {
    // A fresh Backpack with copies of StarterPack every spawn, like Roblox.
    Instance* backpack = scene_->find(c.backpackId);
    Instance* starter = scene_->service(ClassId::StarterPack);
    if (!backpack || !starter) return;
    for (Instance* item : std::vector<Instance*>(backpack->children)) scene_->destroy(item);
    for (Instance* item : std::vector<Instance*>(starter->children)) {
        if (Instance* copy = scene_->cloneSubtree(*item)) scene_->setParent(copy, backpack);
    }
    c.slots.clear();
    c.equipped = 0;
    updateSlots(c);
}

void GameSession::start() {
    if (started_) return;
    started_ = true;
    for (auto& c : clients_) {
        if (options_.spawnCharacter) spawnCharacter(*c);
        else {
            fillPlayerGui(*c, false);
            fillBackpack(*c);
        }
    }
    lua_->start();
    for (auto& c : clients_) {
        c->lua->start();
        c->gui->layout(c->view.size);
    }
}

void GameSession::setFocus(int index) {
    index = std::clamp(index, 0, clientCount() - 1);
    if (index == focus_) return;
    stopAllSounds(); // the new player hears its own world
    for (auto& c : clients_) c->activating = false;
    focus_ = index;
}

GameSession::Client* GameSession::clientOfPlayer(uint64_t playerId) {
    for (auto& c : clients_) if (c->playerId == playerId) return c.get();
    return nullptr;
}

GameSession::Client* GameSession::clientOfCharacter(uint64_t characterId) {
    if (!characterId) return nullptr;
    for (auto& c : clients_) if (c->characterId == characterId) return c.get();
    return nullptr;
}

GameSession::Client* GameSession::clientOfHumanoid(uint64_t humanoidId) {
    if (!humanoidId) return nullptr;
    for (auto& c : clients_) if (c->humanoidId == humanoidId) return c.get();
    return nullptr;
}

void GameSession::setMouseLockRequested(int client, bool lock) {
    if (client >= 0 && client < clientCount()) clients_[static_cast<size_t>(client)]->scriptMouseLock = lock;
}

void GameSession::postRemote(RemoteMessage message) { remotes_.push_back(std::move(message)); }

void GameSession::deliverRemotes() {
    std::vector<RemoteMessage> batch;
    batch.swap(remotes_);
    for (const RemoteMessage& m : batch) {
        if (m.to == NetworkSide::Server) lua_->onRemoteMessage(m);
        else if (m.client >= 0 && m.client < clientCount()) clients_[static_cast<size_t>(m.client)]->lua->onRemoteMessage(m);
    }
}

// ============================================================================
//  Frame
// ============================================================================
void GameSession::update(float dt, const ViewportInput& view) {
    for (auto& c : clients_) {
        if (c->index == focus_) c->view = view;
        else {
            // Players not in focus share the view's size but get no input.
            c->view = view;
            c->view.hovered = false;
            c->view.keyboard = false;
            c->view.mouse = view.size * 0.5f;
        }
    }
    if (!started_) return;
    if (messageTimer_ > 0.0f) {
        messageTimer_ -= dt;
        if (messageTimer_ <= 0.0f && outcome_ == Outcome::None) message_.clear();
    }
    for (auto& c : clients_) c->gui->layout(c->view.size);
    if (paused_ || outcome_ != Outcome::None) {
        for (auto& c : clients_) updateCamera(*c, 0.0f);
        return;
    }
    time_ += dt;

    for (auto& c : clients_) c->gui->handleMouse(c->view.mouse, c->view.hovered, c->view.hovered && input_.mouseDown(GLFW_MOUSE_BUTTON_LEFT));
    handleInput(focused());
    for (auto& c : clients_) c->lua->beforePhysics(dt);
    lua_->beforePhysics(dt);
    for (auto& c : clients_) updateCharacter(*c, dt);
    physics_->step(dt);
    afterPhysics(dt);
    for (auto& c : clients_) c->replicator->syncTransforms();
    lua_->afterPhysics(dt);
    for (auto& c : clients_) c->lua->afterPhysics(dt);
    deliverRemotes();
    for (auto& c : clients_) updateCamera(*c, dt);
    updateSounds();
    for (auto& c : clients_) {
        c->gui->layout(c->view.size); // scripts may have changed the GUI this frame
        updateSlots(*c);
    }
}

// The Backpack bar: number keys and clicks equip, the left button uses the
// tool, Backspace drops it.
void GameSession::handleInput(Client& c) {
    updateSlots(c);
    const std::vector<HotbarSlot> bar = hotbar(c.index);
    auto toggle = [&](const HotbarSlot& slot) {
        if (slot.equipped) unequipTools(c.index);
        else equipTool(c.index, slot.tool);
    };
    if (c.view.keyboard && !c.dead) {
        for (const HotbarSlot& slot : bar) {
            const int key = slot.number == 0 ? GLFW_KEY_0 : GLFW_KEY_1 + slot.number - 1;
            if (input_.keyPressed(key)) toggle(slot);
        }
        if (input_.keyPressed(GLFW_KEY_BACKSPACE) && c.equipped) dropTool(c.index);
    }
    bool overBar = false;
    if (c.view.hovered) {
        for (const HotbarSlot& slot : bar) {
            const glm::vec2 m = c.view.mouse;
            if (m.x < slot.pos.x || m.y < slot.pos.y || m.x > slot.pos.x + slot.size.x || m.y > slot.pos.y + slot.size.y) continue;
            overBar = true;
            if (input_.mousePressed(GLFW_MOUSE_BUTTON_LEFT) && !c.dead) toggle(slot);
        }
    }
    // Using the held tool.
    Instance* tool = c.equipped ? scene_->find(c.equipped) : nullptr;
    if (tool && !c.activating && c.view.hovered && input_.mousePressed(GLFW_MOUSE_BUTTON_LEFT) && !overBar && !c.gui->mouseOverGui()
        && tool->enabled && !tool->manualActivationOnly) {
        c.activating = true;
        activateTool(tool->id, true);
    }
    if (c.activating && !input_.mouseDown(GLFW_MOUSE_BUTTON_LEFT)) {
        c.activating = false;
        if (tool) activateTool(tool->id, false);
    }
}

// ============================================================================
//  Player / character
// ============================================================================
Instance* GameSession::player() const { return scene_->find(focused().playerId); }
Instance* GameSession::character() const { return focused().characterId ? scene_->find(focused().characterId) : nullptr; }
Instance* GameSession::humanoid() const { return focused().humanoidId ? scene_->find(focused().humanoidId) : nullptr; }
Instance* GameSession::rootPart() const { return focused().rootPartId ? scene_->find(focused().rootPartId) : nullptr; }

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

void GameSession::spawnCharacter(Client& c) {
    destroyCharacter(c);
    for (uint64_t id : c.deathPieces) if (Instance* piece = scene_->find(id)) scene_->destroy(piece);
    c.deathPieces.clear();
    Instance* playerInst = scene_->find(c.playerId);
    Instance& starter = scene_->starterPlayer();
    Instance* model = nullptr;

    // A custom character: StarterPlayer.StarterCharacter (a Model with a HumanoidRootPart and a Humanoid).
    if (Instance* custom = starter.findFirstChild("StarterCharacter"); custom && custom->cls == ClassId::Model) {
        Instance* root = custom->findFirstChild("HumanoidRootPart");
        bool hasHumanoid = false;
        for (Instance* child : custom->children) hasHumanoid |= child->cls == ClassId::Humanoid;
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
    // StarterCharacterScripts: LocalScripts run on the client, Scripts on the server.
    for (Instance* folder : std::vector<Instance*>(starter.children)) {
        if (folder->cls != ClassId::StarterCharacterScripts) continue;
        for (Instance* item : std::vector<Instance*>(folder->children)) {
            if (Instance* copy = scene_->cloneSubtree(*item)) scene_->setParent(copy, model);
        }
    }

    Instance* root = model->findFirstChild("HumanoidRootPart");
    Instance* hum = nullptr;
    for (Instance* child : model->children) if (child->cls == ClassId::Humanoid) hum = child;
    hum->walkSpeed = starter.walkSpeed;
    hum->jumpPower = starter.jumpPower;
    hum->health = hum->maxHealth;

    // The model's frame is the root part's centre; the parts hang off it.
    // Several players spawn side by side.
    const glm::vec3 feet = spawnPosition() + glm::vec3(static_cast<float>(c.index) * 1.6f, 0.0f, 0.0f);
    model->position = feet + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f);
    model->rotation = glm::vec3(0.0f, c.facingYaw, 0.0f);
    if (root->position != glm::vec3(0.0f)) {
        // Custom characters: whatever layout they have, the root is the reference.
        model->position -= root->position;
    }
    physics_->setIgnored(model->id, true); // the controller moves it, not rigid-body physics
    scene_->setParent(model, &scene_->workspace());
    c.replicator->setLocalCharacter(model->id); // the client owns its character
    fillPlayerGui(c, c.guiFilled);
    c.guiFilled = true;
    fillBackpack(c);

    c.characterId = model->id;
    c.humanoidId = hum->id;
    c.rootPartId = root->id;
    c.controller = physics_->createCharacter(root->id, feet, kCharacterRadius, kCharacterHeight);
    c.lastHealth = hum->health;
    c.dead = false;
    c.walkTarget = false;
    c.runningSpeed = -1.0f;
    c.armRaised = false;

    // Camera starts behind the new character.
    c.camera.yaw = c.facingYaw;
    if (started_) {
        const uint64_t playerId = c.playerId;
        forAll([&](LuaRuntime& l) { l.onCharacterAdded(playerId, *model); });
    }
}

void GameSession::destroyCharacter(Client& c) {
    if (c.controller) {
        physics_->destroyCharacter(c.controller);
        c.controller = nullptr;
    }
    if (Instance* model = c.characterId ? scene_->find(c.characterId) : nullptr) {
        const uint64_t playerId = c.playerId;
        forAll([&](LuaRuntime& l) { l.onCharacterRemoving(playerId, *model); });
        scene_->destroy(model);
    }
    c.replicator->setLocalCharacter(0);
    c.characterId = c.humanoidId = c.rootPartId = 0;
    c.equipped = 0;
    c.activating = false;
    c.toolOffsets.clear();
}

void GameSession::loadCharacter(uint64_t playerId) {
    if (Client* c = playerId ? clientOfPlayer(playerId) : &focused()) spawnCharacter(*c);
}

void GameSession::moveCharacterTo(uint64_t humanoidId, const glm::vec3& target) {
    if (Client* c = clientOfHumanoid(humanoidId)) {
        c->walkTarget = true;
        c->walkTo = target;
    }
}

void GameSession::updateCharacter(Client& c, float dt) {
    if (!c.controller) {
        if (c.dead && options_.spawnCharacter) {
            c.respawnTimer -= dt;
            if (c.respawnTimer <= 0.0f) spawnCharacter(c);
        }
        return;
    }
    Instance* model = scene_->find(c.characterId);
    Instance* hum = scene_->find(c.humanoidId);
    Instance* root = scene_->find(c.rootPartId);
    if (!model || !hum || !root) { // a script destroyed (part of) the character
        physics_->destroyCharacter(c.controller);
        c.controller = nullptr;
        if (model) scene_->destroy(model);
        c.characterId = c.humanoidId = c.rootPartId = 0;
        c.equipped = 0;
        c.dead = true;
        c.respawnTimer = kRespawnDelay;
        return;
    }

    // A script moved the character (CFrame / PivotTo / Position): teleport the capsule.
    const glm::vec3 expected = c.controller->feetPosition() + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f);
    const glm::vec3 actual = root->worldPosition();
    if (glm::length(actual - expected) > 0.01f) c.controller->teleport(actual - glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f));
    // A script set the root part's velocity (jump pads, knock-back).
    if (glm::length(root->velocity - c.controller->velocity()) > 0.01f) c.controller->setVelocity(root->velocity);

    CharacterController::Input in;
    in.walkSpeed = hum->walkSpeed;
    in.jumpSpeed = hum->jumpPower;
    if (!c.dead) {
        glm::vec3 wish(0.0f);
        if (c.view.keyboard) {
            glm::vec3 forward = c.camera.lookVector();
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
            c.walkTarget = false;
        } else if (c.walkTarget) {
            glm::vec3 to = c.walkTo - c.controller->feetPosition();
            to.y = 0.0f;
            if (glm::length(to) < 0.3f) c.walkTarget = false;
            else wish = glm::normalize(to);
        }
        if (hum->jump) { in.jump = true; hum->jump = false; }
        in.moveDirection = wish;
    }
    hum->moveDirection = in.moveDirection;
    c.controller->setInput(in);
}

void GameSession::afterPhysics(float dt) {
    for (const TouchEvent& e : physics_->takeTouchEvents()) forAll([&](LuaRuntime& l) { l.onTouch(e); });
    for (uint64_t id : physics_->takeFallenParts()) {
        if (Instance* i = scene_->find(id)) scene_->destroy(i); // fell out of the world
    }
    for (auto& c : clients_) {
        placeCharacter(*c, dt);
        updateTools(*c);
    }
    pickUpTools();
}

void GameSession::placeCharacter(Client& c, float dt) {
    Instance* model = c.characterId ? scene_->find(c.characterId) : nullptr;
    Instance* hum = c.humanoidId ? scene_->find(c.humanoidId) : nullptr;
    Instance* root = c.rootPartId ? scene_->find(c.rootPartId) : nullptr;
    if (!c.controller || !model || !hum || !root) return;

    // Place the model on the capsule and turn it towards where it walks.
    const glm::vec3 feet = c.controller->feetPosition();
    const glm::vec3 move = hum->moveDirection;
    const bool firstPerson = scene_->starterPlayer().cameraMode == CameraMode::FirstPerson || c.camera.zoom < 1.0f;
    float targetYaw = c.facingYaw;
    if (firstPerson) targetYaw = c.camera.yaw;
    else if (glm::length(move) > 0.1f) targetYaw = glm::degrees(std::atan2(-move.x, -move.z));
    const float turn = wrapDegrees(targetYaw - c.facingYaw);
    c.facingYaw = wrapDegrees(c.facingYaw + turn * std::min(1.0f, dt * 12.0f));
    const glm::vec3 rootOffset = root->position; // custom characters may have an offset root
    model->setWorldFrame(feet + glm::vec3(0.0f, kCharacterHeight * 0.5f, 0.0f) - rootOffset, glm::vec3(0.0f, c.facingYaw, 0.0f));
    root->velocity = c.controller->velocity();

    if (c.controller->justJumped()) forAll([&](LuaRuntime& l) { l.onJumping(*hum); });
    const float speed = glm::length(glm::vec2(root->velocity.x, root->velocity.z));
    if (std::abs(speed - c.runningSpeed) > 0.5f || (speed == 0.0f && c.runningSpeed != 0.0f)) {
        c.runningSpeed = speed < 0.05f ? 0.0f : speed;
        forAll([&](LuaRuntime& l) { l.onRunning(*hum, c.runningSpeed); });
    }

    // Health and death.
    if (feet.y < scene_->workspace().fallenPartsHeight && hum->health > 0.0f) scene_->setProperty(*hum, "Health", 0.0f);
    if (hum->health > hum->maxHealth) hum->health = hum->maxHealth;
    if (hum->health != c.lastHealth) {
        c.lastHealth = hum->health;
        forAll([&](LuaRuntime& l) { l.onHealthChanged(*hum, hum->health); });
    }
    if (hum->health <= 0.0f && !c.dead) {
        c.dead = true;
        c.respawnTimer = kRespawnDelay;
        c.activating = false;
        forAll([&](LuaRuntime& l) { l.onHumanoidDied(*hum); });
        c.controller->setInput({});
        // Fall apart: the limbs drop (they become ordinary unanchored parts).
        physics_->destroyCharacter(c.controller);
        c.controller = nullptr;
        physics_->setIgnored(model->id, false);
        for (Instance* part : model->children) {
            if (!part->isBasePart()) continue;
            part->canCollide = part->name != "HumanoidRootPart";
            part->velocity = glm::vec3(std::sin(part->position.x * 7.0f), 3.0f, std::cos(part->position.x * 5.0f)) * 2.0f;
        }
        // Detach the parts so each falls on its own; they are removed when
        // the next character spawns.
        for (Instance* part : std::vector<Instance*>(model->children)) {
            if (!part->isBasePart() || part->name == "HumanoidRootPart") continue;
            const glm::mat4 world = part->worldFrame();
            scene_->setParent(part, &scene_->workspace());
            part->setWorldTransform(world * glm::scale(glm::mat4(1.0f), part->size));
            part->archivable = false;
            c.deathPieces.push_back(part->id);
        }
        c.equipped = 0;
        c.toolOffsets.clear();
    }
}

// ============================================================================
//  Tools
// ============================================================================
Instance* GameSession::toolOwnerCharacter(const Instance& tool) const {
    for (const auto& c : clients_) {
        if (tool.parent && tool.parent->id == c->characterId && c->characterId) return tool.parent;
    }
    return nullptr;
}

void GameSession::updateSlots(Client& c) {
    // The bar keeps the order tools arrived in; new ones go at the end.
    std::vector<uint64_t> present;
    uint64_t held = 0;
    if (Instance* backpack = scene_->find(c.backpackId)) {
        for (Instance* item : backpack->children) if (item->cls == ClassId::Tool) present.push_back(item->id);
    }
    if (Instance* model = c.characterId ? scene_->find(c.characterId) : nullptr) {
        for (Instance* item : model->children) {
            if (item->cls != ClassId::Tool) continue;
            present.push_back(item->id);
            if (!held) held = item->id;
        }
    }
    c.slots.erase(std::remove_if(c.slots.begin(), c.slots.end(), [&](uint64_t id) {
        return std::find(present.begin(), present.end(), id) == present.end();
    }), c.slots.end());
    for (uint64_t id : present) {
        if (std::find(c.slots.begin(), c.slots.end(), id) == c.slots.end()) c.slots.push_back(id);
    }
    if (held != c.equipped) {
        // A script parented a tool into (or out of) the character.
        c.equipped = held;
        c.toolOffsets.clear();
        if (Instance* tool = held ? scene_->find(held) : nullptr) {
            if (Instance* handle = handleOf(*tool)) {
                const glm::mat4 inv = glm::inverse(handle->worldFrame());
                Scene::forEachDescendant(*tool, [&](Instance& part) {
                    if (part.isBasePart() && &part != handle && !part.isDescendantOf(handle)) c.toolOffsets[part.id] = inv * part.worldFrame();
                });
            }
        }
    }
}

std::vector<GameSession::HotbarSlot> GameSession::hotbar(int client) const {
    std::vector<HotbarSlot> bar;
    if (client < 0 || client >= clientCount()) return bar;
    const Client& c = *clients_[static_cast<size_t>(client)];
    if (!c.backpackBar || c.slots.empty()) return bar;
    const int count = std::min(static_cast<int>(c.slots.size()), kHotbarSlots);
    // Slots grow with the view (about 60 px on a 1080p screen).
    const float size = std::clamp(c.view.size.y * 0.058f, 40.0f, 84.0f);
    const float gap = size * 0.1f, margin = size * 0.25f;
    const float width = count * size + (count - 1) * gap;
    const glm::vec2 start((c.view.size.x - width) * 0.5f, c.view.size.y - size - margin);
    for (int k = 0; k < count; ++k) {
        HotbarSlot slot;
        slot.tool = c.slots[static_cast<size_t>(k)];
        slot.number = (k + 1) % 10;
        slot.equipped = slot.tool == c.equipped;
        slot.pos = start + glm::vec2(k * (size + gap), 0.0f);
        slot.size = glm::vec2(size);
        bar.push_back(slot);
    }
    return bar;
}

void GameSession::equipTool(int client, uint64_t toolId) {
    if (client < 0 || client >= clientCount()) return;
    Client& c = *clients_[static_cast<size_t>(client)];
    Instance* tool = scene_->find(toolId);
    Instance* model = c.characterId ? scene_->find(c.characterId) : nullptr;
    if (!tool || tool->cls != ClassId::Tool || !model || c.dead || tool->parent == model) return;
    // Only its own tools, or one lying in the world.
    if (Instance* owner = toolOwnerCharacter(*tool); owner && owner != model) return;
    if (tool->parent && tool->parent->cls == ClassId::Backpack && tool->parent->id != c.backpackId) return;
    unequipTools(client);
    if (tool->requiresHandle && !handleOf(*tool)) log(LogLevel::Warning, tool->fullName() + " has no Handle part to hold (or set RequiresHandle off)");
    scene_->setParent(tool, model);
    updateSlots(c);
    forAll([&](LuaRuntime& l) {
        if (l.side() == NetworkSide::Server || l.client() == client) l.onToolEvent(*tool, ToolEvent::Equipped);
    });
}

void GameSession::unequipTools(int client) {
    if (client < 0 || client >= clientCount()) return;
    Client& c = *clients_[static_cast<size_t>(client)];
    Instance* tool = c.equipped ? scene_->find(c.equipped) : nullptr;
    Instance* backpack = scene_->find(c.backpackId);
    if (c.activating && tool) activateTool(tool->id, false);
    c.activating = false;
    if (!tool || !backpack) return;
    scene_->setParent(tool, backpack);
    updateSlots(c);
    forAll([&](LuaRuntime& l) {
        if (l.side() == NetworkSide::Server || l.client() == client) l.onToolEvent(*tool, ToolEvent::Unequipped);
    });
}

void GameSession::dropTool(int client) {
    if (client < 0 || client >= clientCount()) return;
    Client& c = *clients_[static_cast<size_t>(client)];
    Instance* tool = c.equipped ? scene_->find(c.equipped) : nullptr;
    Instance* root = c.rootPartId ? scene_->find(c.rootPartId) : nullptr;
    if (!tool || !tool->canBeDropped || !root) return;
    if (c.activating) activateTool(tool->id, false);
    c.activating = false;
    forAll([&](LuaRuntime& l) {
        if (l.side() == NetworkSide::Server || l.client() == client) l.onToolEvent(*tool, ToolEvent::Unequipped);
    });
    // In front of the character, where it falls.
    Instance* handle = handleOf(*tool);
    const glm::mat4 before = handle ? handle->worldFrame() : glm::mat4(1.0f);
    const float yaw = glm::radians(c.facingYaw);
    const glm::vec3 ahead = root->worldPosition() + glm::vec3(-std::sin(yaw), 0.3f, -std::cos(yaw)) * 2.5f;
    scene_->setParent(tool, &scene_->workspace());
    if (handle) {
        const glm::mat4 after = glm::translate(glm::mat4(1.0f), ahead) * glm::mat4(glm::mat3(before));
        Scene::forEachDescendant(*tool, [&](Instance& part) {
            if (!part.isBasePart() || part.parent != tool) return;
            setWorldMatrix(part, after * glm::inverse(before) * part.worldFrame());
            if (const PropertyInfo* p = findProperty(part.cls, "Position")) scene_->notifyPropertyChanged(part, *p);
        });
    }
    pickupBlockedUntil_[tool->id] = time_ + 2.0;
    c.equipped = 0;
    c.toolOffsets.clear();
    updateSlots(c);
}

void GameSession::activateTool(uint64_t toolId, bool down) {
    Instance* tool = scene_->find(toolId);
    if (!tool || tool->cls != ClassId::Tool) return;
    Instance* owner = toolOwnerCharacter(*tool);
    Client* c = owner ? clientOfCharacter(owner->id) : nullptr;
    if (down && !tool->enabled) return;
    forAll([&](LuaRuntime& l) {
        if (l.side() == NetworkSide::Server || (c && l.client() == c->index)) l.onToolEvent(*tool, down ? ToolEvent::Activated : ToolEvent::Deactivated);
    });
}

void GameSession::updateTools(Client& c) {
    Instance* model = c.characterId ? scene_->find(c.characterId) : nullptr;
    Instance* tool = c.equipped ? scene_->find(c.equipped) : nullptr;
    Instance* arm = model ? model->findFirstChild("Right Arm") : nullptr;
    if (arm && !arm->isBasePart()) arm = nullptr;

    // The right arm holds the tool out in front (until animations take over).
    const bool hold = tool && !c.dead && arm;
    if (hold && !c.armRaised) {
        c.armRestPos = arm->position;
        c.armRestRot = arm->rotation;
        const float half = arm->size.y * 0.5f;
        const glm::vec3 shoulder = arm->position + glm::vec3(0.0f, half, 0.0f);
        arm->position = shoulder + glm::vec3(0.0f, 0.0f, -half);
        arm->rotation = glm::vec3(90.0f, 0.0f, 0.0f);
        c.armRaised = true;
    } else if (!hold && c.armRaised) {
        if (arm) {
            arm->position = c.armRestPos;
            arm->rotation = c.armRestRot;
        }
        c.armRaised = false;
    }
    if (!tool || !model || c.dead) return;
    Instance* handle = handleOf(*tool);
    if (!handle) return;

    // Roblox's grip: the bottom of the arm, turned so the tool points forward,
    // then the tool's Grip (where the Handle sits in the hand).
    const glm::mat4 hand = arm ? arm->worldFrame() * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -arm->size.y * 0.5f, 0.0f))
                                     * glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f))
                               : model->worldFrame();
    const glm::mat4 grip = TransformUtil::compose(tool->gripPos, tool->gripRotation, glm::vec3(1.0f));
    const glm::mat4 handleWorld = hand * glm::inverse(grip);
    setWorldMatrix(*handle, handleWorld);
    for (const auto& [id, offset] : c.toolOffsets) {
        if (Instance* part = scene_->find(id)) setWorldMatrix(*part, handleWorld * offset);
    }
}

void GameSession::pickUpTools() {
    // Walking into a tool lying in the world picks it up (and holds it if the hand is free).
    for (auto& cp : clients_) {
        Client& c = *cp;
        Instance* root = c.rootPartId && !c.dead ? scene_->find(c.rootPartId) : nullptr;
        if (!root) continue;
        for (uint64_t id : physics_->partsInBox(root->worldPosition(), glm::vec3(2.2f, kCharacterHeight + 0.4f, 2.2f))) {
            Instance* part = scene_->find(id);
            if (!part || part->name != "Handle" || !part->parent || part->parent->cls != ClassId::Tool) continue;
            Instance* tool = part->parent;
            if (!tool->isDescendantOf(&scene_->workspace()) || toolOwnerCharacter(*tool)) continue;
            bool insideCharacter = false;
            for (const auto& other : clients_) insideCharacter |= other->characterId && tool->isDescendantOf(scene_->find(other->characterId));
            if (insideCharacter) continue;
            auto blocked = pickupBlockedUntil_.find(tool->id);
            if (blocked != pickupBlockedUntil_.end()) {
                if (blocked->second > time_) continue;
                pickupBlockedUntil_.erase(blocked);
            }
            if (!c.equipped) equipTool(c.index, tool->id);
            else if (Instance* backpack = scene_->find(c.backpackId)) {
                scene_->setParent(tool, backpack);
                updateSlots(c);
            }
        }
    }
}

// ============================================================================
//  Camera
// ============================================================================
void GameSession::updateCamera(Client& c, float dt) {
    (void)dt;
    GameCamera& camera = c.camera;
    const bool firstPersonMode = scene_->starterPlayer().cameraMode == CameraMode::FirstPerson;
    const float maxZoom = std::max(1.0f, scene_->starterPlayer().cameraMaxZoom);
    if (firstPersonMode) camera.zoom = 0.5f;
    const bool firstPerson = camera.zoom < 1.0f;
    const bool rotating = c.view.hovered && input_.mouseDown(GLFW_MOUSE_BUTTON_RIGHT);
    c.mouseLock = (firstPerson && c.view.hovered) || (c.scriptMouseLock && c.index == focus_);

    if (camera.type == CameraType::Custom) {
        if (rotating || (c.mouseLock && c.index == focus_)) {
            const glm::vec2 d = input_.mouseDelta();
            camera.yaw = wrapDegrees(camera.yaw - d.x * 0.3f);
            camera.pitch = std::clamp(camera.pitch - d.y * 0.3f, -80.0f, 80.0f);
        }
        if (c.view.hovered && input_.scroll() != 0.0f && !firstPersonMode) {
            camera.zoom = std::clamp(camera.zoom * (1.0f - input_.scroll() * 0.12f), 0.5f, maxZoom);
            if (camera.zoom < 1.0f && input_.scroll() < 0.0f) camera.zoom = 1.0f; // zoom back out of first person
        }
        const Instance* root = c.rootPartId ? scene_->find(c.rootPartId) : nullptr;
        glm::vec3 focus = root ? root->worldPosition() + glm::vec3(0.0f, 0.8f, 0.0f) : camera.position;
        if (!root && c.dead) focus = camera.position + camera.lookVector() * camera.zoom;
        const float yaw = glm::radians(camera.yaw), pitch = glm::radians(camera.pitch);
        const glm::vec3 forward(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
        float distance = firstPerson ? 0.0f : camera.zoom;
        if (distance > 0.0f && root) {
            // Walls between the character and the camera pull it closer.
            RaycastFilter filter;
            filter.respectCanCollide = true;
            if (c.characterId) filter.instances.push_back(c.characterId);
            if (auto hit = physics_->raycast(focus, -forward * (distance + 0.3f), filter)) distance = std::max(0.3f, hit->distance - 0.3f);
        }
        camera.position = focus - forward * distance;
        camera.lookAt(camera.position, camera.position + forward);
    }
    if (options_.audio && c.index == focus_) options_.audio->setListener(camera.position, camera.lookVector(), camera.upVector());
}

bool GameSession::screenPointToRay(int client, const glm::vec2& point, glm::vec3& origin, glm::vec3& direction) const {
    if (client < 0 || client >= clientCount()) return false;
    const Client& c = *clients_[static_cast<size_t>(client)];
    if (c.view.size.x <= 0.0f || c.view.size.y <= 0.0f) return false;
    const glm::vec2 ndc(2.0f * point.x / c.view.size.x - 1.0f, 1.0f - 2.0f * point.y / c.view.size.y);
    const glm::mat4 inv = glm::inverse(c.camera.projection(c.view.size.x / c.view.size.y) * c.camera.view());
    glm::vec4 nearP = inv * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 farP = inv * glm::vec4(ndc, 1.0f, 1.0f);
    nearP /= nearP.w;
    farP /= farP.w;
    origin = glm::vec3(nearP);
    direction = glm::normalize(glm::vec3(farP - nearP));
    return true;
}

glm::vec3 GameSession::worldToScreen(int client, const glm::vec3& world) const {
    if (client < 0 || client >= clientCount()) return glm::vec3(0.0f);
    const Client& c = *clients_[static_cast<size_t>(client)];
    const glm::vec4 clip = c.camera.projection(c.view.size.x / std::max(1.0f, c.view.size.y)) * c.camera.view() * glm::vec4(world, 1.0f);
    if (std::abs(clip.w) < 1e-6f) return { 0.0f, 0.0f, 0.0f };
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return { (ndc.x + 1.0f) * 0.5f * c.view.size.x, (1.0f - ndc.y) * 0.5f * c.view.size.y, clip.w };
}

// ============================================================================
//  Output
// ============================================================================
void GameSession::log(LogLevel level, const std::string& text, uint64_t script, NetworkSide side, int client) {
    log_.push_back({ text, level, time_, script, side, client });
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
//  What plays is the focused player's world.
// ============================================================================
void GameSession::playSound(Instance& sound) {
    if (sound.scene) sound.scene->setProperty(sound, "Playing", true); // a server sound replicates to the clients
    auto it = sounds_.find(sound.id);
    if (it != sounds_.end() && options_.audio) {
        options_.audio->stop(it->second.handle); // :Play() restarts from the beginning
        sounds_.erase(it);
    }
    updateSounds();
}

void GameSession::stopSound(Instance& sound) {
    if (sound.scene) sound.scene->setProperty(sound, "Playing", false);
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

void GameSession::stopAllSounds() {
    if (options_.audio) {
        for (auto& [id, s] : sounds_) options_.audio->stop(s.handle);
    }
    sounds_.clear();
}

void GameSession::updateSounds() {
    AudioSystem* audio = options_.audio;
    Scene& world = clientScene();
    const Instance* soundService = world.service(ClassId::SoundService);
    const float master = soundService ? soundService->volume : 1.0f;
    if (audio) audio->setMasterVolume(master);

    // Start sounds whose Playing is on.
    std::vector<Instance*> wanted;
    world.forEachInTree([&](Instance& i) { if (i.cls == ClassId::Sound && i.playing) wanted.push_back(&i); });
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
        Instance* s = world.find(it->first);
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
                if (Instance* server = scene_->find(s->id)) server->playing = false;
                forAll([&](LuaRuntime& l) { l.onSoundEnded(*s); });
                it = sounds_.erase(it);
                continue;
            }
        } else if (!audio || !it->second.handle) {
            // No audio device (or no file): the sound "ends" at once, so scripts waiting on Ended continue.
            s->playing = false;
            if (Instance* server = scene_->find(s->id)) server->playing = false;
            forAll([&](LuaRuntime& l) { l.onSoundEnded(*s); });
            it = sounds_.erase(it);
            continue;
        }
        ++it;
    }
}

} // namespace rift
