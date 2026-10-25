#pragma once
//
// Instance.h -- every object in a RIFT scene is an Instance (Roblox naming):
// parts, models, folders, scripts, lights, sounds, GUI elements and the
// services at the root (Workspace, Lighting, ...).
//
// DESIGN: one plain struct holds the fields of every class. Classes reuse
// fields where the meaning matches (Color, Enabled, Brightness, ...), and the
// reflection tables (Reflection.h) decide which fields each class exposes and
// under which property names. That keeps instances trivially copyable for
// Play-mode cloning and undo, with no virtual dispatch anywhere.
//
// Ownership: a Scene owns its instances. `parent`/`children` are non-owning.
// Code outside the scene layer refers to instances by `id` (stable across
// save/load, undo and Play) and resolves it with Scene::find when needed.
//
#include "scene/Value.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace rift {

class Scene;

enum class ClassId : uint8_t {
    // Services: exactly one of each per scene, always at the root.
    Workspace, Players, Lighting, ReplicatedStorage, ServerStorage, ScriptService, StarterGui, StarterPack, StarterPlayer, SoundService,
    // World
    Part, SpawnLocation, MeshPart, Model, Folder, Tool, Terrain,
    PointLight, SpotLight,
    // Effects: points on parts, and what is drawn from them
    Attachment, ParticleEmitter, Beam, Trail,
    Sound,
    Humanoid,
    // Players and their containers (made by the engine)
    Player, PlayerGui, PlayerScripts, Backpack, StarterPlayerScripts, StarterCharacterScripts,
    // Values (data holders, e.g. leaderstats)
    IntValue, NumberValue, StringValue, BoolValue, Vector3Value, Color3Value,
    // Code
    Script, LocalScript, ModuleScript,
    // Communication: client <-> server (Remote) and script <-> script (Bindable)
    RemoteEvent, RemoteFunction, BindableEvent, BindableFunction,
    // 2D GUI
    ScreenGui, Frame, TextLabel, TextButton, ImageLabel, ImageButton,
    Count
};
constexpr int kClassCount = static_cast<int>(ClassId::Count);

// ---- enums used by properties (their names live in Reflection.cpp)
enum class PartShape : int { Block, Ball, Cylinder, Wedge, CornerWedge };
// Roblox's materials. Parts use any of them; terrain voxels use the natural
// ones (Grass .. CrackedLava, plus Concrete, Brick, Cobblestone, WoodPlanks,
// Ice, Water). Saved by name, so new ones can be added anywhere.
enum class Material : int {
    Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete,
    Brick, Cobblestone, Granite, Marble, Slate, Fabric, Foil, DiamondPlate, CorrodedMetal, WoodPlanks, Pebble, Ice, ForceField,
    Grass, LeafyGrass, Sand, Sandstone, Rock, Ground, Mud, Snow, Glacier, Basalt, Asphalt, Limestone, Pavement, Salt, CrackedLava,
    Water, Air,
    Count
};
constexpr int kMaterialCount = static_cast<int>(Material::Count);
enum class CameraMode : int { ThirdPerson, FirstPerson };
enum class TextXAlignment : int { Left, Center, Right };
enum class TextYAlignment : int { Top, Center, Bottom };
enum class ScaleType : int { Stretch, Fit, Crop };
enum class NormalId : int { Right, Top, Back, Left, Bottom, Front };   // +X, +Y, +Z, -X, -Y, -Z
enum class ParticleOrientation : int { FacingCamera, FacingCameraWorldUp, VelocityParallel, VelocityPerpendicular };
enum class TextureMode : int { Stretch, Wrap, Static };

struct Attribute {
    std::string name;
    Value value;
};

struct Instance {
    // ------------------------------------------------------------ identity
    uint64_t id = 0;
    ClassId cls = ClassId::Folder;
    std::string name;
    Instance* parent = nullptr;
    std::vector<Instance*> children;
    Scene* scene = nullptr;
    bool archivable = true;               // false: not saved and not cloned
    std::vector<Attribute> attributes;    // user-defined values (SetAttribute)
    std::vector<std::string> tags;        // CollectionService tags

    // ------------------------------------------------------------ transform (parts, models, lights)
    glm::vec3 position{ 0.0f };
    glm::vec3 rotation{ 0.0f };           // Euler degrees, applied X then Y then Z
    glm::vec3 size{ 1.0f };               // part size / model scale

    // ------------------------------------------------------------ appearance
    PartShape shape = PartShape::Block;
    Color3 color{ 0.64f, 0.64f, 0.64f };  // part colour, light colour, GUI background
    Material material = Material::Plastic;
    float transparency = 0.0f;
    float reflectance = 0.0f;
    AssetPath texture;                    // part surface texture / ImageLabel image
    glm::vec2 textureScale{ 1.0f };       // texture repeats per stud (0 = stretch once)
    bool castShadow = true;               // also Lighting.GlobalShadows / light Shadows
    bool visible = true;
    AssetPath mesh;                       // MeshPart: model file (+ "#n" sub-mesh)
    AssetPath soundId;                    // Sound: audio file

    // ------------------------------------------------------------ physics
    bool anchored = true;
    bool canCollide = true;
    bool canTouch = true;
    float density = 1.0f;
    float friction = 0.5f;
    float elasticity = 0.0f;
    glm::vec3 velocity{ 0.0f };           // runtime: linear velocity
    glm::vec3 angularVelocity{ 0.0f };    // runtime: degrees per second

    // ------------------------------------------------------------ behaviour
    bool enabled = true;                  // scripts, lights, ScreenGui, sounds' autoplay ...
    std::string source;                   // Script/ModuleScript: file relative to the scene folder

    // ------------------------------------------------------------ lights / sounds / environment
    float brightness = 1.0f;
    float range = 16.0f;
    float angle = 45.0f;                  // spot cone angle (degrees)
    float volume = 0.5f;
    float playbackSpeed = 1.0f;
    bool looped = false;
    bool playing = false;                 // sound: plays when the game starts
    float rollOffDistance = 60.0f;

    // Services (fields only meaningful on the matching service)
    float gravity = 30.0f;                // Workspace
    float fallenPartsHeight = -60.0f;     // Workspace: parts below this are destroyed
    float clockTime = 14.0f;              // Lighting: 0..24, drives the sun
    Color3 ambient{ 0.42f, 0.44f, 0.48f };
    Color3 skyColor{ 0.30f, 0.53f, 0.92f };
    Color3 horizonColor{ 0.70f, 0.80f, 0.94f };
    Color3 fogColor{ 0.78f, 0.86f, 0.96f };
    float fogStart = 0.0f;                // Roblox defaults: no fog until FogEnd is lowered
    float fogEnd = 100000.0f;
    float exposure = 1.0f;
    float walkSpeed = 8.0f;               // StarterPlayer, Humanoid
    float jumpPower = 12.0f;
    float health = 100.0f;                // Humanoid
    float maxHealth = 100.0f;
    glm::vec3 moveDirection{ 0.0f };      // Humanoid (runtime): where the character is walking
    bool jump = false;                    // Humanoid (runtime): set to make it jump

    // Tool (ToolTip is `text`, TextureId is `texture`, Enabled is `enabled`)
    bool requiresHandle = true;           // a Part named Handle is what the hand holds
    bool canBeDropped = true;             // Backspace drops it into the world
    bool manualActivationOnly = false;    // clicks do not activate it (scripts call :Activate())
    glm::vec3 gripPos{ 0.0f };            // where the hand holds the Handle (Handle space, studs)
    glm::vec3 gripRotation{ 0.0f };       // how the Handle is turned in the hand (degrees)

    // Value objects (IntValue.Value, ...). StringValue uses `text`, Color3Value `color`.
    int intValue = 0;
    float numberValue = 0.0f;
    bool boolValue = false;
    glm::vec3 vector3Value{ 0.0f };
    CameraMode cameraMode = CameraMode::ThirdPerson;
    float cameraMaxZoom = 40.0f;

    // ------------------------------------------------------------ effects
    // Attachment: Position / Orientation are `position` / `rotation`, in the
    // parent part's space. ParticleEmitter, Beam and Trail share Enabled,
    // Texture and Brightness; their colour and transparency change over a
    // particle's life or along the beam / trail, so they are sequences.
    ColorSequence colorSequence;                 // Color
    NumberSequence transparencySequence{ 0.0f }; // Transparency
    NumberSequence sizeSequence{ 1.0f };         // ParticleEmitter.Size, Trail.WidthScale
    NumberRange lifetime{ 5.0f, 10.0f };         // ParticleEmitter.Lifetime (seconds)
    NumberRange speedRange{ 5.0f };              // ParticleEmitter.Speed (studs/s)
    NumberRange rotationRange{ 0.0f };           // ParticleEmitter.Rotation (degrees at birth)
    NumberRange rotSpeed{ 0.0f };                // ParticleEmitter.RotSpeed (degrees/s)
    float rate = 20.0f;                          // particles per second
    glm::vec2 spreadAngle{ 0.0f };               // degrees around the emission direction (X, Y)
    glm::vec3 acceleration{ 0.0f };              // studs/s^2
    float drag = 0.0f;                           // speed lost per second (exponential)
    float lightEmission = 0.0f;                  // 0 normal blending, 1 additive (glows)
    float lightInfluence = 0.0f;                 // how much the scene's light darkens it
    float zOffset = 0.0f;                        // studs towards the camera
    float timeScale = 1.0f;
    bool lockedToPart = false;                   // particles move with the emitter
    NormalId emissionDirection = NormalId::Top;
    ParticleOrientation particleOrientation = ParticleOrientation::FacingCamera;
    InstanceRef attachment0, attachment1;        // Beam, Trail: the two Attachments
    float width0 = 1.0f, width1 = 1.0f;          // Beam: width at each end
    float curveSize0 = 0.0f, curveSize1 = 0.0f;  // Beam: bend along each attachment's X axis
    int segments = 10;                           // Beam: pieces along the curve
    bool faceCamera = false;                     // Beam, Trail: always turned towards the camera
    float textureLength = 1.0f;                  // studs per texture repeat (Wrap)
    float textureSpeed = 1.0f;                   // Beam: texture scroll (repeats per second)
    TextureMode textureMode = TextureMode::Stretch;
    float trailLifetime = 2.0f;                  // Trail.Lifetime (seconds)
    float minLength = 0.1f, maxLength = 0.0f;    // Trail: point spacing, total length (0 = no limit)
    // Runtime (never saved): ParticleEmitter:Emit(n) / :Clear() waiting for the renderer.
    int emitRequests = 0;
    uint32_t clearRequests = 0;

    // ------------------------------------------------------------ GUI
    UDim2 guiPosition{};
    UDim2 guiSize{ 0.0f, 200.0f, 0.0f, 50.0f };
    glm::vec2 anchorPoint{ 0.0f };
    float backgroundTransparency = 0.0f;
    int zIndex = 1;
    int displayOrder = 0;
    bool resetOnSpawn = true;             // ScreenGui: copied into PlayerGui again when the character respawns
    float cornerRadius = 0.0f;
    float borderSize = 0.0f;
    Color3 borderColor{ 0.1f, 0.1f, 0.1f };
    std::string text;
    Color3 textColor{ 0.1f, 0.1f, 0.1f };
    float textSize = 18.0f;
    TextXAlignment textXAlignment = TextXAlignment::Center;
    TextYAlignment textYAlignment = TextYAlignment::Center;
    bool textWrapped = false;
    Color3 imageColor{ 1.0f, 1.0f, 1.0f };
    float imageTransparency = 0.0f;
    ScaleType scaleType = ScaleType::Stretch;

    // ------------------------------------------------------------ queries
    bool isA(ClassId c) const { return cls == c; }
    bool isService() const;
    bool isBasePart() const;      // Part, SpawnLocation, MeshPart: has geometry and physics
    bool hasTransform() const;    // anything with position/rotation (parts, models, lights)
    bool isScript() const;        // Script, LocalScript or ModuleScript
    bool isRemote() const;        // RemoteEvent, RemoteFunction, BindableEvent, BindableFunction
    // Containers the engine keeps (StarterPlayerScripts, PlayerGui, ...): like
    // services, they cannot be deleted, moved or duplicated in the editor.
    bool isFixedContainer() const;
    bool isGui() const;           // GUI element (ScreenGui or a GuiObject)
    bool isGuiObject() const;     // drawable GUI element (not ScreenGui)
    bool isLight() const;
    bool isValueObject() const;   // IntValue, NumberValue, ...

    // Transforms. Only position and rotation are inherited down the tree
    // ("frame"); `size` scales the part itself and never its children, so a
    // child's Size is always its real size (as in Roblox and Godot).
    glm::mat4 localFrame() const;      // T(position) * R(rotation)
    glm::mat4 worldFrame() const;      // composed with every ancestor that has a transform
    glm::mat4 parentWorldFrame() const;
    glm::mat4 worldTransform() const;  // worldFrame() * S(size) for parts, worldFrame() otherwise
    glm::vec3 worldPosition() const;
    glm::vec3 worldRotation() const;   // Euler degrees in world space
    // Sets the world frame (position + rotation); size is left unchanged.
    void setWorldFrame(const glm::vec3& worldPos, const glm::vec3& worldRotDeg);
    // Sets everything from a world matrix (e.g. the gizmo): frame + size.
    void setWorldTransform(const glm::mat4& world);

    bool isDescendantOf(const Instance* ancestor) const;
    Instance* findFirstChild(const std::string& childName) const;

    // Attributes (nil Value removes it).
    const Value* attribute(const std::string& attrName) const;
    void setAttribute(const std::string& attrName, const Value& v);
    bool hasTag(const std::string& tag) const;
    void addTag(const std::string& tag);
    void removeTag(const std::string& tag);

    // "Workspace.Model.Part" (for messages).
    std::string fullName() const;
};

// Identity helpers: ids are random 64-bit values printed as 16 hex digits.
uint64_t newInstanceId();
std::string idToString(uint64_t id);
uint64_t idFromString(const std::string& s); // 0 if malformed

} // namespace rift
