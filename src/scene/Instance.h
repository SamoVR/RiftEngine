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
    Workspace, Lighting, ReplicatedStorage, ScriptService, StarterGui, StarterPlayer, SoundService,
    // World
    Part, SpawnLocation, MeshPart, Model, Folder,
    PointLight, SpotLight,
    Sound,
    // Code
    Script, ModuleScript,
    // 2D GUI
    ScreenGui, Frame, TextLabel, TextButton, ImageLabel, ImageButton,
    Count
};
constexpr int kClassCount = static_cast<int>(ClassId::Count);

// ---- enums used by properties (their names live in Reflection.cpp)
enum class PartShape : int { Block, Ball, Cylinder, Wedge, CornerWedge };
enum class Material : int { Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete };
enum class CameraMode : int { ThirdPerson, FirstPerson };
enum class TextXAlignment : int { Left, Center, Right };
enum class TextYAlignment : int { Top, Center, Bottom };
enum class ScaleType : int { Stretch, Fit, Crop };

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
    Color3 skyColor{ 0.36f, 0.58f, 0.92f };
    Color3 horizonColor{ 0.78f, 0.86f, 0.96f };
    Color3 fogColor{ 0.78f, 0.86f, 0.96f };
    float fogStart = 120.0f;
    float fogEnd = 600.0f;
    float exposure = 1.0f;
    float walkSpeed = 8.0f;               // StarterPlayer
    float jumpPower = 12.0f;
    CameraMode cameraMode = CameraMode::ThirdPerson;
    float cameraMaxZoom = 40.0f;

    // ------------------------------------------------------------ GUI
    UDim2 guiPosition{};
    UDim2 guiSize{ 0.0f, 200.0f, 0.0f, 50.0f };
    glm::vec2 anchorPoint{ 0.0f };
    float backgroundTransparency = 0.0f;
    int zIndex = 1;
    int displayOrder = 0;
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
    bool isScript() const;        // Script or ModuleScript
    bool isGui() const;           // GUI element (ScreenGui or a GuiObject)
    bool isGuiObject() const;     // drawable GUI element (not ScreenGui)
    bool isLight() const;

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
