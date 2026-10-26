#include "scene/Reflection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace rift {

namespace {

// ---------------------------------------------------------------- enum tables
const std::vector<std::string> kPartTypes = { "Block", "Ball", "Cylinder", "Wedge", "CornerWedge" };
const std::vector<std::string> kMaterials = {
    "Plastic", "SmoothPlastic", "Metal", "Neon", "Glass", "Wood", "Concrete",
    "Brick", "Cobblestone", "Granite", "Marble", "Slate", "Fabric", "Foil", "DiamondPlate", "CorrodedMetal", "WoodPlanks", "Pebble", "Ice", "ForceField",
    "Grass", "LeafyGrass", "Sand", "Sandstone", "Rock", "Ground", "Mud", "Snow", "Glacier", "Basalt", "Asphalt", "Limestone", "Pavement", "Salt", "CrackedLava",
    "Water", "Air" };
const std::vector<std::string> kCameraModes = { "ThirdPerson", "FirstPerson" };
const std::vector<std::string> kTextX = { "Left", "Center", "Right" };
const std::vector<std::string> kTextY = { "Top", "Center", "Bottom" };
const std::vector<std::string> kScaleTypes = { "Stretch", "Fit", "Crop" };
const std::vector<std::string> kFonts = { "Gotham", "GothamMedium", "GothamBold", "GothamBlack", "SourceSans", "SourceSansSemibold", "SourceSansBold", "Arial", "ArialBold", "Code" };
const std::vector<std::string> kNormalIds = { "Right", "Top", "Back", "Left", "Bottom", "Front" };
const std::vector<std::string> kParticleOrientations = { "FacingCamera", "FacingCameraWorldUp", "VelocityParallel", "VelocityPerpendicular" };
const std::vector<std::string> kTextureModes = { "Stretch", "Wrap", "Static" };
const std::vector<std::string> kAnimationPriorities = { "Idle", "Movement", "Action", "Action2", "Action3", "Action4", "Core" };
const std::vector<std::string> kPoseEasingStyles = { "Linear", "Constant", "Elastic", "Cubic", "Bounce", "CubicV2" };
const std::vector<std::string> kPoseEasingDirections = { "In", "Out", "InOut" };

// ---------------------------------------------------------------- field accessors
template <class T> constexpr ValueType valueTypeOf() {
    if constexpr (std::is_same_v<T, bool>) return ValueType::Bool;
    else if constexpr (std::is_same_v<T, int>) return ValueType::Int;
    else if constexpr (std::is_same_v<T, float>) return ValueType::Float;
    else if constexpr (std::is_same_v<T, std::string>) return ValueType::String;
    else if constexpr (std::is_same_v<T, glm::vec3>) return ValueType::Vector3;
    else if constexpr (std::is_same_v<T, glm::vec2>) return ValueType::Vector2;
    else if constexpr (std::is_same_v<T, Color3>) return ValueType::Color3;
    else if constexpr (std::is_same_v<T, UDim2>) return ValueType::UDim2;
    else if constexpr (std::is_same_v<T, AssetPath>) return ValueType::Asset;
    else if constexpr (std::is_same_v<T, NumberRange>) return ValueType::NumberRange;
    else if constexpr (std::is_same_v<T, NumberSequence>) return ValueType::NumberSequence;
    else if constexpr (std::is_same_v<T, ColorSequence>) return ValueType::ColorSequence;
    else if constexpr (std::is_same_v<T, InstanceRef>) return ValueType::Ref;
    else if constexpr (std::is_same_v<T, CFrameValue>) return ValueType::CFrame;
    else if constexpr (std::is_enum_v<T>) return ValueType::Enum;
    else static_assert(sizeof(T) == 0, "unsupported property field type");
}

template <auto M> using FieldType = std::remove_cvref_t<decltype(std::declval<Instance&>().*M)>;

template <auto M> Value getField(const Instance& i) {
    if constexpr (std::is_enum_v<FieldType<M>>) return EnumValue{ static_cast<int>(i.*M) };
    else return Value(i.*M);
}

template <auto M> bool setField(Instance& i, const Value& v) {
    using T = FieldType<M>;
    if constexpr (std::is_enum_v<T>) {
        if (const EnumValue* e = std::get_if<EnumValue>(&v)) { i.*M = static_cast<T>(e->index); return true; }
        return false;
    } else {
        if (const T* x = std::get_if<T>(&v)) { i.*M = *x; return true; }
        return false;
    }
}

template <auto M> PropertyInfo field(const char* name, const char* category) {
    PropertyInfo p;
    p.name = name;
    p.category = category;
    p.type = valueTypeOf<FieldType<M>>();
    p.get = &getField<M>;
    p.set = &setField<M>;
    return p;
}

// Name is the one property every class has, but it is not a plain field
// setter: services keep their fixed names.
Value getName(const Instance& i) { return i.name; }
bool setName(Instance& i, const Value& v) {
    const std::string* s = std::get_if<std::string>(&v);
    if (!s || i.isService()) return false;
    i.name = *s;
    return true;
}
PropertyInfo nameProp() {
    PropertyInfo p;
    p.name = "Name";
    p.category = "Data";
    p.type = ValueType::String;
    p.get = &getName;
    p.set = &setName;
    p.tooltip = "Label in the Explorer; scripts find children by name (part.Name or parent:FindFirstChild).";
    return p;
}

// ---------------------------------------------------------------- property groups
void addTransform(std::vector<PropertyInfo>& p, bool withSize, const char* sizeName = "Size") {
    p.push_back(field<&Instance::position>("Position", "Transform").speed(0.05f).tip("Position relative to the parent part or model (studs)."));
    p.push_back(field<&Instance::rotation>("Rotation", "Transform").speed(0.5f).tip("Rotation in degrees (X, then Y, then Z)."));
    if (withSize) p.push_back(field<&Instance::size>(sizeName, "Transform").range(0.01f, 4096.0f).speed(0.02f).tip("Size in studs. Never affects children."));
}

void addPartAppearance(std::vector<PropertyInfo>& p, bool withShape) {
    if (withShape) p.push_back(field<&Instance::shape>("Shape", "Appearance").enums("PartType"));
    p.push_back(field<&Instance::color>("Color", "Appearance"));
    p.push_back(field<&Instance::material>("Material", "Appearance").enums("Material").tip("Surface look: Neon glows, Glass is see-through and shiny, Metal is reflective."));
    p.push_back(field<&Instance::transparency>("Transparency", "Appearance").range(0.0f, 1.0f).speed(0.01f));
    p.push_back(field<&Instance::reflectance>("Reflectance", "Appearance").range(0.0f, 1.0f).speed(0.01f));
    p.push_back(field<&Instance::texture>("Texture", "Appearance").tip("Image drawn on every face (multiplied by Color)."));
    p.push_back(field<&Instance::textureScale>("TextureScale", "Appearance").range(0.0f, 1000.0f).speed(0.05f).tip("Studs per texture repeat. 0 stretches the image once over each face."));
    p.push_back(field<&Instance::castShadow>("CastShadow", "Appearance"));
    p.push_back(field<&Instance::visible>("Visible", "Appearance").tip("Invisible parts still collide (unless CanCollide is off)."));
}

void addPartPhysics(std::vector<PropertyInfo>& p) {
    p.push_back(field<&Instance::anchored>("Anchored", "Physics").tip("Anchored parts never move by physics. Turn off to make the part fall, roll and be pushed."));
    p.push_back(field<&Instance::canCollide>("CanCollide", "Physics").tip("Off: other parts and the player pass through it (Touched still fires)."));
    p.push_back(field<&Instance::canTouch>("CanTouch", "Physics").tip("Off: Touched events never fire for this part."));
    p.push_back(field<&Instance::density>("Density", "Physics").range(0.01f, 100.0f).speed(0.05f));
    p.push_back(field<&Instance::friction>("Friction", "Physics").range(0.0f, 2.0f).speed(0.01f));
    p.push_back(field<&Instance::elasticity>("Elasticity", "Physics").range(0.0f, 1.0f).speed(0.01f).tip("Bounciness: 0 = no bounce, 1 = perfectly elastic."));
    p.push_back(field<&Instance::velocity>("Velocity", "Physics").flagsSet(PF_ScriptRead | PF_ScriptWrite | PF_Runtime));
    p.push_back(field<&Instance::angularVelocity>("AngularVelocity", "Physics").flagsSet(PF_ScriptRead | PF_ScriptWrite | PF_Runtime));
}

void addGuiObject(std::vector<PropertyInfo>& p) {
    p.push_back(field<&Instance::guiPosition>("Position", "Layout").tip("{scale, offset} per axis: scale is a fraction of the parent, offset is in pixels."));
    p.push_back(field<&Instance::guiSize>("Size", "Layout"));
    p.push_back(field<&Instance::anchorPoint>("AnchorPoint", "Layout").range(0.0f, 1.0f).speed(0.01f).tip("Which point of the element sits at Position: 0,0 top-left, 0.5,0.5 centre."));
    p.push_back(field<&Instance::zIndex>("ZIndex", "Layout").range(-100.0f, 1000.0f));
    p.push_back(field<&Instance::visible>("Visible", "Layout"));
    p.push_back(field<&Instance::guiRotation>("Rotation", "Layout").range(-3600.0f, 3600.0f).tip("Degrees, clockwise, around the element's centre (clicks still use the unrotated box)."));
    p.push_back(field<&Instance::color>("BackgroundColor3", "Appearance"));
    p.push_back(field<&Instance::backgroundTransparency>("BackgroundTransparency", "Appearance").range(0.0f, 1.0f).speed(0.01f));
    p.push_back(field<&Instance::cornerRadius>("CornerRadius", "Appearance").range(0.0f, 500.0f).tip("Rounded corners, in pixels."));
    p.push_back(field<&Instance::borderSize>("BorderSizePixel", "Appearance").range(0.0f, 50.0f));
    p.push_back(field<&Instance::borderColor>("BorderColor3", "Appearance"));
}

void addText(std::vector<PropertyInfo>& p) {
    p.push_back(field<&Instance::text>("Text", "Text"));
    p.push_back(field<&Instance::textColor>("TextColor3", "Text"));
    p.push_back(field<&Instance::textSize>("TextSize", "Text").range(4.0f, 200.0f));
    p.push_back(field<&Instance::textXAlignment>("TextXAlignment", "Text").enums("TextXAlignment"));
    p.push_back(field<&Instance::textYAlignment>("TextYAlignment", "Text").enums("TextYAlignment"));
    p.push_back(field<&Instance::textWrapped>("TextWrapped", "Text"));
    p.push_back(field<&Instance::textScaled>("TextScaled", "Text").tip("The text grows or shrinks to fill the element (TextSize is ignored)."));
    p.push_back(field<&Instance::font>("Font", "Text").enums("Font"));
    p.push_back(field<&Instance::textTransparency>("TextTransparency", "Text").range(0.0f, 1.0f).speed(0.01f));
    p.push_back(field<&Instance::textStrokeColor>("TextStrokeColor3", "Text"));
    p.push_back(field<&Instance::textStrokeTransparency>("TextStrokeTransparency", "Text").range(0.0f, 1.0f).speed(0.01f).tip("An outline around the letters: 0 = solid, 1 = none."));
}

// Attachment.WorldPosition: computed from the parent part; setting it moves the attachment.
Value getWorldPosition(const Instance& i) { return i.worldPosition(); }
bool setWorldPosition(Instance& i, const Value& v) {
    const glm::vec3* p = std::get_if<glm::vec3>(&v);
    if (!p) return false;
    i.setWorldFrame(*p, i.worldRotation());
    return true;
}

// What ParticleEmitter, Beam and Trail share.
void addEffectLook(std::vector<PropertyInfo>& p, const char* transparencyTip) {
    p.push_back(field<&Instance::colorSequence>("Color", "Appearance"));
    p.push_back(field<&Instance::transparencySequence>("Transparency", "Appearance").range(0.0f, 1.0f).tip(transparencyTip));
    p.push_back(field<&Instance::texture>("Texture", "Appearance").tip("Image to draw (white parts take the colour). Empty: a soft dot for particles, a plain band for beams and trails."));
    p.push_back(field<&Instance::brightness>("Brightness", "Appearance").range(0.0f, 20.0f).speed(0.02f));
    p.push_back(field<&Instance::lightEmission>("LightEmission", "Appearance").range(0.0f, 1.0f).speed(0.01f).tip("0: drawn normally. 1: adds its light to what is behind it (fire, magic, lasers)."));
    p.push_back(field<&Instance::lightInfluence>("LightInfluence", "Appearance").range(0.0f, 1.0f).speed(0.01f).tip("0: always fully bright. 1: lit by the sun and lights like a part."));
}

void addAttachments(std::vector<PropertyInfo>& p) {
    p.push_back(field<&Instance::attachment0>("Attachment0", "Attachments").tip("Where it starts: an Attachment inside a part."));
    p.push_back(field<&Instance::attachment1>("Attachment1", "Attachments").tip("Where it ends: another Attachment."));
}

void addImage(std::vector<PropertyInfo>& p) {
    p.push_back(field<&Instance::texture>("Image", "Image"));
    p.push_back(field<&Instance::imageColor>("ImageColor3", "Image"));
    p.push_back(field<&Instance::imageTransparency>("ImageTransparency", "Image").range(0.0f, 1.0f).speed(0.01f));
    p.push_back(field<&Instance::scaleType>("ScaleType", "Image").enums("ScaleType"));
}

std::vector<ClassInfo> buildClasses() {
    std::vector<ClassInfo> classes(kClassCount);
    auto def = [&](ClassId id, const char* name, const char* description, bool creatable) -> ClassInfo& {
        ClassInfo& c = classes[static_cast<int>(id)];
        c.id = id;
        c.name = name;
        c.description = description;
        c.creatable = creatable;
        c.service = static_cast<int>(id) <= static_cast<int>(ClassId::SoundService);
        c.props.push_back(nameProp());
        if (c.service) c.props.back().flags |= PF_ReadOnlyUI;
        return c;
    };

    // ------------------------------------------------------------ services
    {
        ClassInfo& c = def(ClassId::Workspace, "Workspace", "The 3D world. Everything in it is drawn and simulated.", false);
        c.props.push_back(field<&Instance::gravity>("Gravity", "Physics").range(0.0f, 1000.0f).tip("Downward acceleration in studs/s^2."));
        c.props.push_back(field<&Instance::fallenPartsHeight>("FallenPartsDestroyHeight", "Physics").tip("Parts that fall below this height are destroyed; the player respawns."));
    }
    {
        ClassInfo& c = def(ClassId::Lighting, "Lighting", "Sun, sky, ambient light and fog.", false);
        c.props.push_back(field<&Instance::clockTime>("ClockTime", "Sun").range(0.0f, 24.0f).speed(0.05f).tip("Time of day: moves the sun and tints the sky."));
        c.props.push_back(field<&Instance::brightness>("Brightness", "Sun").range(0.0f, 10.0f).speed(0.02f));
        c.props.push_back(field<&Instance::color>("SunColor", "Sun"));
        c.props.push_back(field<&Instance::castShadow>("GlobalShadows", "Sun"));
        c.props.push_back(field<&Instance::ambient>("Ambient", "Sky"));
        c.props.push_back(field<&Instance::skyColor>("SkyColor", "Sky"));
        c.props.push_back(field<&Instance::horizonColor>("HorizonColor", "Sky"));
        c.props.push_back(field<&Instance::exposure>("ExposureCompensation", "Sky").range(-3.0f, 3.0f).speed(0.01f));
        c.props.push_back(field<&Instance::fogColor>("FogColor", "Fog"));
        c.props.push_back(field<&Instance::fogStart>("FogStart", "Fog").range(0.0f, 100000.0f));
        c.props.push_back(field<&Instance::fogEnd>("FogEnd", "Fog").range(0.0f, 100000.0f));
    }
    def(ClassId::Players, "Players", "The players in the running game (empty while editing).", false);
    def(ClassId::ReplicatedStorage, "ReplicatedStorage", "Shared by the server and the client: modules, RemoteEvents, templates that scripts Clone(). Scripts here do not run.", false);
    def(ClassId::ServerStorage, "ServerStorage", "Server-only storage: never sent to the client. Scripts here do not run.", false);
    def(ClassId::ScriptService, "ScriptService", "Server scripts that run for the whole game (Roblox: ServerScriptService). Never sent to the client.", false);
    def(ClassId::StarterGui, "StarterGui", "Copied into each player's PlayerGui when the game starts (and on respawn): ScreenGuis and the LocalScripts that drive them.", false);
    def(ClassId::StarterPack, "StarterPack", "Tools here go into every player's Backpack each time the character spawns.", false);
    {
        ClassInfo& c = def(ClassId::StarterPlayer, "StarterPlayer", "Player character and camera settings.", false);
        c.props.push_back(field<&Instance::walkSpeed>("WalkSpeed", "Character").range(0.0f, 200.0f));
        c.props.push_back(field<&Instance::jumpPower>("JumpPower", "Character").range(0.0f, 200.0f));
        c.props.push_back(field<&Instance::color>("CharacterColor", "Character"));
        c.props.push_back(field<&Instance::cameraMode>("CameraMode", "Camera").enums("CameraMode"));
        c.props.push_back(field<&Instance::cameraMaxZoom>("CameraMaxZoomDistance", "Camera").range(1.0f, 1000.0f));
    }
    {
        ClassInfo& c = def(ClassId::SoundService, "SoundService", "Sounds that are not attached to a part (music, UI sounds).", false);
        c.props.push_back(field<&Instance::volume>("Volume", "Sound").range(0.0f, 2.0f).speed(0.01f).tip("Master volume."));
    }

    // ------------------------------------------------------------ world
    {
        ClassInfo& c = def(ClassId::Part, "Part", "A basic building block: block, ball, cylinder or wedge.", true);
        addTransform(c.props, true);
        addPartAppearance(c.props, true);
        addPartPhysics(c.props);
    }
    {
        ClassInfo& c = def(ClassId::SpawnLocation, "SpawnLocation", "The player appears on top of an enabled SpawnLocation.", true);
        addTransform(c.props, true);
        addPartAppearance(c.props, false);
        addPartPhysics(c.props);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior"));
    }
    {
        ClassInfo& c = def(ClassId::MeshPart, "MeshPart", "A part whose shape comes from an imported 3D model.", true);
        addTransform(c.props, true);
        c.props.push_back(field<&Instance::mesh>("MeshId", "Appearance").tip("Model file (.obj, .gltf, .glb) in the project's assets."));
        addPartAppearance(c.props, false);
        addPartPhysics(c.props);
    }
    {
        ClassInfo& c = def(ClassId::Model, "Model", "Groups parts so they move together.", true);
        addTransform(c.props, false);
    }
    def(ClassId::Folder, "Folder", "Organises instances. Has no position of its own.", true);
    {
        ClassInfo& c = def(ClassId::Terrain, "Terrain", "Smooth voxel terrain (workspace.Terrain): sculpt it with the Terrain Editor, or from scripts with FillBlock, FillBall, ...", false);
        c.props.push_back(field<&Instance::color>("WaterColor", "Water"));
        c.props.push_back(field<&Instance::transparency>("WaterTransparency", "Water").range(0.0f, 1.0f).speed(0.01f));
    }
    {
        ClassInfo& c = def(ClassId::Tool, "Tool", "Something a player holds: a Part named Handle inside, the tool in StarterPack (or lying in the world to be picked up).", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior").tip("Off: clicking does not activate it."));
        c.props.push_back(field<&Instance::requiresHandle>("RequiresHandle", "Behavior").tip("Off: a tool with no Handle (a spell, an ability)."));
        c.props.push_back(field<&Instance::canBeDropped>("CanBeDropped", "Behavior").tip("On: Backspace drops it into the world."));
        c.props.push_back(field<&Instance::manualActivationOnly>("ManualActivationOnly", "Behavior").tip("On: clicks do not activate it; scripts call :Activate()."));
        c.props.push_back(field<&Instance::text>("ToolTip", "Appearance").tip("Shown when the mouse is over its slot in the backpack bar."));
        c.props.push_back(field<&Instance::texture>("TextureId", "Appearance").tip("Icon in the backpack bar (an image file). Without one the bar shows the name."));
        c.props.push_back(field<&Instance::gripPos>("GripPos", "Grip").speed(0.02f).tip("Where the hand holds the Handle, in the Handle's own space (studs)."));
        c.props.push_back(field<&Instance::gripRotation>("GripRotation", "Grip").speed(0.5f).tip("How the Handle is turned in the hand (degrees)."));
    }
    {
        ClassInfo& c = def(ClassId::PointLight, "PointLight", "Light shining in every direction.", true);
        c.props.push_back(field<&Instance::position>("Position", "Transform").speed(0.05f));
        c.props.push_back(field<&Instance::enabled>("Enabled", "Light"));
        c.props.push_back(field<&Instance::color>("Color", "Light"));
        c.props.push_back(field<&Instance::brightness>("Brightness", "Light").range(0.0f, 50.0f).speed(0.05f));
        c.props.push_back(field<&Instance::range>("Range", "Light").range(0.0f, 200.0f).speed(0.1f));
        c.props.push_back(field<&Instance::castShadow>("Shadows", "Light"));
    }
    {
        ClassInfo& c = def(ClassId::SpotLight, "SpotLight", "Cone of light, pointing along its -Z axis (rotate it to aim).", true);
        addTransform(c.props, false);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Light"));
        c.props.push_back(field<&Instance::color>("Color", "Light"));
        c.props.push_back(field<&Instance::brightness>("Brightness", "Light").range(0.0f, 50.0f).speed(0.05f));
        c.props.push_back(field<&Instance::range>("Range", "Light").range(0.0f, 200.0f).speed(0.1f));
        c.props.push_back(field<&Instance::angle>("Angle", "Light").range(1.0f, 170.0f));
        c.props.push_back(field<&Instance::castShadow>("Shadows", "Light"));
    }
    {
        ClassInfo& c = def(ClassId::Attachment, "Attachment", "A point (with a direction) on a part. Beams and trails go between two; particles can come out of one.", true);
        c.props.push_back(field<&Instance::position>("Position", "Transform").speed(0.05f).tip("Offset from the middle of the parent part, in its own space (studs)."));
        c.props.push_back(field<&Instance::rotation>("Orientation", "Transform").speed(0.5f).tip("Rotation in degrees. Its X axis is where a beam curves; particles leave along its Y axis."));
        c.props.push_back(field<&Instance::visible>("Visible", "Appearance").tip("Show it in the editor even when it is not selected."));
        PropertyInfo world;
        world.name = "WorldPosition";
        world.category = "Transform";
        world.type = ValueType::Vector3;
        world.get = &getWorldPosition;
        world.set = &setWorldPosition;
        world.flags = PF_ScriptRead | PF_ScriptWrite | PF_Runtime;
        c.props.push_back(world);
    }
    {
        ClassInfo& c = def(ClassId::ParticleEmitter, "ParticleEmitter", "Sends out little images: fire, smoke, sparkles, rain, dust. Put it in a part (they come out of a face) or an Attachment.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Emission").tip("Off: no new particles (the ones alive finish their life). Scripts can still :Emit(n)."));
        c.props.push_back(field<&Instance::rate>("Rate", "Emission").range(0.0f, 1000.0f).speed(0.2f).tip("Particles per second."));
        c.props.push_back(field<&Instance::lifetime>("Lifetime", "Emission").range(0.0f, 60.0f).tip("How long each particle lives, in seconds (random between the two)."));
        c.props.push_back(field<&Instance::speedRange>("Speed", "Emission").range(-500.0f, 500.0f).tip("Starting speed in studs per second (random between the two)."));
        c.props.push_back(field<&Instance::spreadAngle>("SpreadAngle", "Emission").range(-180.0f, 180.0f).speed(0.5f).tip("How far (degrees) particles may leave from the emission direction, on two axes."));
        c.props.push_back(field<&Instance::emissionDirection>("EmissionDirection", "Emission").enums("NormalId").tip("Which face of the part they come out of (Top = up)."));
        c.props.push_back(field<&Instance::rotationRange>("Rotation", "Emission").range(-360.0f, 360.0f).tip("Starting rotation of the image, in degrees."));
        c.props.push_back(field<&Instance::rotSpeed>("RotSpeed", "Emission").range(-3600.0f, 3600.0f).tip("Spin in degrees per second."));
        addEffectLook(c.props, "Over each particle's life: 0 solid, 1 invisible.");
        c.props.push_back(field<&Instance::sizeSequence>("Size", "Appearance").range(0.0f, 100.0f).tip("Over each particle's life, in studs."));
        c.props.push_back(field<&Instance::particleOrientation>("Orientation", "Appearance").enums("ParticleOrientation").tip("FacingCamera for most effects; VelocityParallel stretches along the motion (rain, sparks)."));
        c.props.push_back(field<&Instance::zOffset>("ZOffset", "Appearance").range(-100.0f, 100.0f).speed(0.05f).tip("Moves the particles towards the camera (studs) so they draw in front of nearby surfaces."));
        c.props.push_back(field<&Instance::acceleration>("Acceleration", "Motion").speed(0.1f).tip("Constant push in studs/s^2, e.g. (0, -30, 0) to fall, (0, 4, 0) for rising smoke."));
        c.props.push_back(field<&Instance::drag>("Drag", "Motion").range(0.0f, 100.0f).speed(0.02f).tip("How quickly particles slow down."));
        c.props.push_back(field<&Instance::lockedToPart>("LockedToPart", "Motion").tip("On: particles move along with the part (a jet flame). Off: they stay where they were born."));
        c.props.push_back(field<&Instance::timeScale>("TimeScale", "Motion").range(0.0f, 10.0f).speed(0.01f).tip("Speeds up or slows down the whole effect (0 freezes it)."));
    }
    {
        ClassInfo& c = def(ClassId::Beam, "Beam", "A band drawn between two Attachments, straight or curved: lasers, ropes, light rays, waterfalls.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior"));
        addAttachments(c.props);
        addEffectLook(c.props, "Along the beam, from Attachment0 (0) to Attachment1 (1): 0 solid, 1 invisible.");
        c.props.push_back(field<&Instance::textureMode>("TextureMode", "Appearance").enums("TextureMode").tip("Stretch: the image once over the beam. Wrap: repeats every TextureLength studs. Static: repeats without stretching."));
        c.props.push_back(field<&Instance::textureLength>("TextureLength", "Appearance").range(0.01f, 1000.0f).speed(0.02f));
        c.props.push_back(field<&Instance::textureSpeed>("TextureSpeed", "Appearance").range(-100.0f, 100.0f).speed(0.02f).tip("How fast the texture scrolls along the beam."));
        c.props.push_back(field<&Instance::width0>("Width0", "Shape").range(0.0f, 1000.0f).speed(0.02f));
        c.props.push_back(field<&Instance::width1>("Width1", "Shape").range(0.0f, 1000.0f).speed(0.02f));
        c.props.push_back(field<&Instance::curveSize0>("CurveSize0", "Shape").range(-1000.0f, 1000.0f).speed(0.05f).tip("Bends the beam out along Attachment0's X axis (studs)."));
        c.props.push_back(field<&Instance::curveSize1>("CurveSize1", "Shape").range(-1000.0f, 1000.0f).speed(0.05f).tip("Bends the beam out along Attachment1's X axis (studs)."));
        c.props.push_back(field<&Instance::segments>("Segments", "Shape").range(1.0f, 1000.0f).tip("Pieces the curve is made of."));
        c.props.push_back(field<&Instance::faceCamera>("FaceCamera", "Shape").tip("On: the band always turns its face to the camera."));
        c.props.push_back(field<&Instance::zOffset>("ZOffset", "Shape").range(-100.0f, 100.0f).speed(0.05f));
    }
    {
        ClassInfo& c = def(ClassId::Trail, "Trail", "A fading ribbon left behind as its two Attachments move: sword swings, speed streaks, magic.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior"));
        addAttachments(c.props);
        addEffectLook(c.props, "From the newest end (0) to the oldest (1): 0 solid, 1 invisible.");
        c.props.push_back(field<&Instance::textureMode>("TextureMode", "Appearance").enums("TextureMode"));
        c.props.push_back(field<&Instance::textureLength>("TextureLength", "Appearance").range(0.01f, 1000.0f).speed(0.02f));
        c.props.push_back(field<&Instance::trailLifetime>("Lifetime", "Emission").range(0.01f, 20.0f).speed(0.01f).tip("Seconds a point of the trail stays before it fades out."));
        c.props.push_back(field<&Instance::minLength>("MinLength", "Emission").range(0.0f, 100.0f).speed(0.01f).tip("A new point is added when the attachments moved this far (studs)."));
        c.props.push_back(field<&Instance::maxLength>("MaxLength", "Emission").range(0.0f, 10000.0f).speed(0.1f).tip("The longest the trail may get (studs). 0: no limit."));
        c.props.push_back(field<&Instance::sizeSequence>("WidthScale", "Shape").range(0.0f, 10.0f).tip("Width along the trail as a fraction of the distance between the attachments."));
        c.props.push_back(field<&Instance::faceCamera>("FaceCamera", "Shape"));
    }
    // ------------------------------------------------------------ animation
    {
        ClassInfo& c = def(ClassId::Motor6D, "Motor6D", "A joint that holds Part1 to Part0 and that animations turn: Part1 = Part0 * C0 * Transform * C1:Inverse().", true);
        c.props.push_back(field<&Instance::attachment0>("Part0", "Parts").tip("The part the joint hangs from (the Torso for a shoulder)."));
        c.props.push_back(field<&Instance::attachment1>("Part1", "Parts").tip("The part the joint moves (the arm). Animations name their poses after it."));
        c.props.push_back(field<&Instance::c0>("C0", "Joint").tip("Where the joint is, in Part0's space."));
        c.props.push_back(field<&Instance::c1>("C1", "Joint").tip("Where the joint is, in Part1's space."));
        c.props.push_back(field<&Instance::enabled>("Enabled", "Joint"));
        c.props.push_back(field<&Instance::jointTransform>("Transform", "Joint").flagsSet(PF_ScriptRead | PF_ScriptWrite | PF_Runtime));
    }
    def(ClassId::Animator, "Animator", "Plays animations on its model: put it in a Humanoid or an AnimationController. Scripts call animator:LoadAnimation(animation).", true);
    def(ClassId::AnimationController, "AnimationController", "Lets a model without a Humanoid play animations (a door, a creature): it holds an Animator.", true);
    {
        ClassInfo& c = def(ClassId::Animation, "Animation", "Points at an animation (AnimationId). Load it with animator:LoadAnimation(animation) to get a track to Play().", true);
        c.props.push_back(field<&Instance::animationId>("AnimationId", "Animation").tip("A .anim file made with the Animation Editor (assets/animations/Wave.anim), an engine animation (engine:animations/walk.anim), or a KeyframeSequence registered with KeyframeSequenceProvider."));
    }
    {
        ClassInfo& c = def(ClassId::KeyframeSequence, "KeyframeSequence", "An animation as objects: Keyframes with Poses. The Animation Editor keeps its work here.", true);
        c.props.push_back(field<&Instance::looped>("Loop", "Animation"));
        c.props.push_back(field<&Instance::priority>("Priority", "Animation").enums("AnimationPriority").tip("Higher priorities override lower ones on the joints they move (Core < Idle < Movement < Action)."));
    }
    {
        ClassInfo& c = def(ClassId::Keyframe, "Keyframe", "One moment of a KeyframeSequence; its Poses say where each joint is then.", true);
        c.props.push_back(field<&Instance::keyframeTime>("Time", "Keyframe").range(0.0f, 1000.0f).speed(0.01f));
    }
    {
        ClassInfo& c = def(ClassId::Pose, "Pose", "Where one joint is in a Keyframe: named after the joint's Part1, with the offset in CFrame.", true);
        c.props.push_back(field<&Instance::c0>("CFrame", "Pose"));
        c.props.push_back(field<&Instance::poseWeight>("Weight", "Pose").range(0.0f, 1.0f).speed(0.01f));
        c.props.push_back(field<&Instance::easingStyle>("EasingStyle", "Pose").enums("PoseEasingStyle").tip("How it moves towards the next keyframe."));
        c.props.push_back(field<&Instance::easingDirection>("EasingDirection", "Pose").enums("PoseEasingDirection"));
    }
    {
        ClassInfo& c = def(ClassId::KeyframeMarker, "KeyframeMarker", "A named event in a Keyframe: track:GetMarkerReachedSignal(name) fires when the animation passes it.", true);
        c.props.push_back(field<&Instance::text>("Value", "Marker"));
    }
    {
        ClassInfo& c = def(ClassId::Sound, "Sound", "Plays an audio file. Inside a part it is 3D (quieter with distance).", true);
        c.props.push_back(field<&Instance::soundId>("SoundId", "Sound").tip("Audio file (.wav, .mp3, .ogg, .flac) in the project's assets."));
        c.props.push_back(field<&Instance::volume>("Volume", "Sound").range(0.0f, 10.0f).speed(0.01f));
        c.props.push_back(field<&Instance::playbackSpeed>("PlaybackSpeed", "Sound").range(0.05f, 10.0f).speed(0.01f));
        c.props.push_back(field<&Instance::looped>("Looped", "Sound"));
        c.props.push_back(field<&Instance::playing>("Playing", "Sound").tip("On: plays as soon as the game starts. Scripts use :Play() / :Stop()."));
        c.props.push_back(field<&Instance::rollOffDistance>("RollOffMaxDistance", "Sound").range(1.0f, 10000.0f));
    }

    {
        ClassInfo& c = def(ClassId::Humanoid, "Humanoid", "Makes a model a character: health, walking and jumping.", true);
        c.props.push_back(field<&Instance::health>("Health", "Health").range(0.0f, 1e9f).tip("At 0 the character dies (Died fires) and respawns."));
        c.props.push_back(field<&Instance::maxHealth>("MaxHealth", "Health").range(0.0f, 1e9f));
        c.props.push_back(field<&Instance::walkSpeed>("WalkSpeed", "Movement").range(0.0f, 500.0f));
        c.props.push_back(field<&Instance::jumpPower>("JumpPower", "Movement").range(0.0f, 500.0f));
        c.props.push_back(field<&Instance::moveDirection>("MoveDirection", "Movement").flagsSet(PF_ScriptRead | PF_Runtime));
        c.props.push_back(field<&Instance::jump>("Jump", "Movement").flagsSet(PF_ScriptRead | PF_ScriptWrite | PF_Runtime));
    }

    {
        ClassInfo& c = def(ClassId::Player, "Player", "A player in the running game (created by the engine).", false);
        c.props.front().flags = PF_ScriptRead | PF_Editor | PF_ReadOnlyUI;
    }
    def(ClassId::PlayerGui, "PlayerGui", "The player's on-screen GUI: copies of StarterGui (in the running game).", false);
    def(ClassId::PlayerScripts, "PlayerScripts", "Client-only: copies of StarterPlayerScripts (in the running game).", false);
    def(ClassId::Backpack, "Backpack", "The player's inventory (in the running game).", false);
    def(ClassId::StarterPlayerScripts, "StarterPlayerScripts", "LocalScripts here run on the client once, when the player joins.", false);
    def(ClassId::StarterCharacterScripts, "StarterCharacterScripts", "Copied into every new character: LocalScripts run on the client, Scripts on the server.", false);

    // ------------------------------------------------------------ values
    def(ClassId::IntValue, "IntValue", "Holds a whole number. In a player's leaderstats folder it is shown on the leaderboard.", true)
        .props.push_back(field<&Instance::intValue>("Value", "Data"));
    def(ClassId::NumberValue, "NumberValue", "Holds a number.", true)
        .props.push_back(field<&Instance::numberValue>("Value", "Data"));
    def(ClassId::StringValue, "StringValue", "Holds text.", true)
        .props.push_back(field<&Instance::text>("Value", "Data"));
    def(ClassId::BoolValue, "BoolValue", "Holds true or false.", true)
        .props.push_back(field<&Instance::boolValue>("Value", "Data"));
    def(ClassId::Vector3Value, "Vector3Value", "Holds a Vector3.", true)
        .props.push_back(field<&Instance::vector3Value>("Value", "Data"));
    def(ClassId::Color3Value, "Color3Value", "Holds a Color3.", true)
        .props.push_back(field<&Instance::color>("Value", "Data"));

    // ------------------------------------------------------------ code
    {
        ClassInfo& c = def(ClassId::Script, "Script", "Lua code that runs while the game plays. Inside a part, script.Parent is that part.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior").tip("A disabled script does not run when the game starts."));
        c.props.push_back(field<&Instance::source>("Source", "Behavior").flagsSet(PF_Serialize | PF_Editor | PF_ReadOnlyUI | PF_ScriptRead).tip("The .lua file, relative to the scene folder."));
    }
    {
        ClassInfo& c = def(ClassId::LocalScript, "LocalScript", "Lua code that runs on the client: input, camera, GUI. Runs inside PlayerGui, PlayerScripts, the character or StarterPlayerScripts.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior").tip("A disabled script does not run when the game starts."));
        c.props.push_back(field<&Instance::source>("Source", "Behavior").flagsSet(PF_Serialize | PF_Editor | PF_ReadOnlyUI | PF_ScriptRead).tip("The .lua file, relative to the scene folder."));
    }
    {
        ClassInfo& c = def(ClassId::ModuleScript, "ModuleScript", "Lua code that returns a value; load it with require(module).", true);
        c.props.push_back(field<&Instance::source>("Source", "Behavior").flagsSet(PF_Serialize | PF_Editor | PF_ReadOnlyUI | PF_ScriptRead));
    }

    // ------------------------------------------------------------ communication
    def(ClassId::RemoteEvent, "RemoteEvent", "Sends messages between the client and the server: FireServer / OnServerEvent, FireClient / OnClientEvent.", true);
    def(ClassId::RemoteFunction, "RemoteFunction", "Calls between the client and the server that return a result: InvokeServer / OnServerInvoke.", true);
    def(ClassId::BindableEvent, "BindableEvent", "Sends messages between scripts on the same side: Fire / Event.", true);
    def(ClassId::BindableFunction, "BindableFunction", "Calls between scripts on the same side: Invoke / OnInvoke.", true);

    // ------------------------------------------------------------ GUI
    {
        ClassInfo& c = def(ClassId::ScreenGui, "ScreenGui", "A layer of 2D interface drawn over the game (put it in StarterGui).", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior"));
        c.props.push_back(field<&Instance::displayOrder>("DisplayOrder", "Behavior").range(-1000.0f, 1000.0f).tip("Higher values are drawn on top of other ScreenGuis."));
        c.props.push_back(field<&Instance::resetOnSpawn>("ResetOnSpawn", "Behavior").tip("On: the player gets a fresh copy every time the character respawns."));
    }
    {
        ClassInfo& c = def(ClassId::Frame, "Frame", "A rectangle; holds other GUI elements.", true);
        addGuiObject(c.props);
    }
    {
        ClassInfo& c = def(ClassId::TextLabel, "TextLabel", "Displays text.", true);
        addGuiObject(c.props);
        addText(c.props);
    }
    {
        ClassInfo& c = def(ClassId::TextButton, "TextButton", "A clickable button with text. Connect to .Activated.", true);
        addGuiObject(c.props);
        addText(c.props);
    }
    {
        ClassInfo& c = def(ClassId::ImageLabel, "ImageLabel", "Displays an image.", true);
        addGuiObject(c.props);
        addImage(c.props);
    }
    {
        ClassInfo& c = def(ClassId::ImageButton, "ImageButton", "A clickable image. Connect to .Activated.", true);
        addGuiObject(c.props);
        addImage(c.props);
    }
    {
        ClassInfo& c = def(ClassId::UIGradient, "UIGradient", "Put inside a GUI element: shades its background, text and image with a gradient.", true);
        c.props.push_back(field<&Instance::enabled>("Enabled", "Behavior"));
        c.props.push_back(field<&Instance::colorSequence>("Color", "Appearance").tip("Multiplies the element's colours, along the gradient."));
        c.props.push_back(field<&Instance::transparencySequence>("Transparency", "Appearance").range(0.0f, 1.0f));
        c.props.push_back(field<&Instance::guiRotation>("Rotation", "Appearance").range(-3600.0f, 3600.0f).tip("0: left to right, 90: top to bottom."));
        c.props.push_back(field<&Instance::gradientOffset>("Offset", "Appearance").range(-1.0f, 1.0f).speed(0.01f).tip("Moves the gradient, in fractions of the element's size."));
    }

    // Every class can carry Archivable.
    for (ClassInfo& c : classes) {
        if (c.service || c.id == ClassId::Player) continue;
        c.props.push_back(field<&Instance::archivable>("Archivable", "Data").tip("Off: not saved with the scene and not copied by Clone()."));
    }
    for (ClassInfo& c : classes) {
        for (size_t i = 0; i < c.props.size(); ++i) c.byName.emplace(c.props[i].name, static_cast<int>(i));
    }
    return classes;
}

const std::vector<ClassInfo>& classes() {
    static const std::vector<ClassInfo> table = buildClasses();
    return table;
}

// Defaults are read from a freshly initialised instance of each class.
struct DefaultCache {
    std::array<std::vector<Value>, kClassCount> values;
    DefaultCache() {
        for (int c = 0; c < kClassCount; ++c) {
            Instance fresh;
            fresh.cls = static_cast<ClassId>(c);
            applyClassDefaults(fresh);
            for (const PropertyInfo& p : classes()[c].props) values[c].push_back(p.get(fresh));
        }
    }
};

} // namespace

PropertyInfo& PropertyInfo::enums(const char* typeName) {
    enumType = typeName;
    enumNames = enumNamesFor(typeName);
    return *this;
}

const PropertyInfo* ClassInfo::find(const std::string& propName) const {
    auto it = byName.find(propName);
    return it == byName.end() ? nullptr : &props[static_cast<size_t>(it->second)];
}

int ClassInfo::indexOf(const std::string& propName) const {
    auto it = byName.find(propName);
    return it == byName.end() ? -1 : it->second;
}

const ClassInfo& classInfo(ClassId cls) { return classes()[static_cast<int>(cls)]; }

const ClassInfo* classByName(const std::string& name) {
    for (const ClassInfo& c : classes()) if (c.name == name) return &c;
    return nullptr;
}

const char* className(ClassId cls) { return classInfo(cls).name.c_str(); }

const PropertyInfo* findProperty(ClassId cls, const std::string& name) { return classInfo(cls).find(name); }

void applyClassDefaults(Instance& i) {
    i.name = classInfo(i.cls).name;
    switch (i.cls) {
        case ClassId::Lighting:
            i.brightness = 1.0f;
            i.color = { 1.0f, 0.96f, 0.88f };
            i.castShadow = true;
            break;
        case ClassId::StarterPlayer:
            i.color = { 0.25f, 0.55f, 1.0f };
            break;
        case ClassId::SoundService:
            i.volume = 1.0f;
            break;
        case ClassId::Player:
            i.archivable = false; // never saved
            break;
        case ClassId::Color3Value:
            i.color = { 1.0f, 1.0f, 1.0f };
            break;
        case ClassId::Terrain:
            i.color = { 12.0f / 255.0f, 84.0f / 255.0f, 92.0f / 255.0f }; // WaterColor
            i.transparency = 0.3f;
            break;
        case ClassId::SpawnLocation:
            i.size = { 4.0f, 0.4f, 4.0f };
            i.color = { 0.30f, 0.80f, 0.45f };
            break;
        case ClassId::Part:
        case ClassId::MeshPart:
            i.size = { 4.0f, 1.0f, 2.0f };
            break;
        case ClassId::PointLight:
            i.color = { 1.0f, 0.95f, 0.85f };
            i.castShadow = false;
            break;
        case ClassId::SpotLight:
            i.color = { 1.0f, 0.95f, 0.85f };
            i.range = 24.0f;
            i.castShadow = false;
            break;
        case ClassId::Attachment:
            i.visible = false;
            break;
        case ClassId::Beam:
            i.transparencySequence = NumberSequence(0.5f);
            break;
        case ClassId::Trail:
            i.transparencySequence = NumberSequence(0.5f);
            break;
        case ClassId::Keyframe:
        case ClassId::Pose:
            break;
        case ClassId::Frame:
            i.color = { 1.0f, 1.0f, 1.0f };
            i.guiSize = { 0.0f, 100.0f, 0.0f, 100.0f };
            break;
        case ClassId::TextLabel:
            i.color = { 1.0f, 1.0f, 1.0f };
            i.text = "Label";
            break;
        case ClassId::TextButton:
            i.color = { 0.25f, 0.55f, 1.0f };
            i.textColor = { 1.0f, 1.0f, 1.0f };
            i.text = "Button";
            i.cornerRadius = 6.0f;
            break;
        case ClassId::ImageLabel:
        case ClassId::ImageButton:
            i.color = { 1.0f, 1.0f, 1.0f };
            i.backgroundTransparency = 1.0f;
            i.guiSize = { 0.0f, 100.0f, 0.0f, 100.0f };
            break;
        default:
            break;
    }
}

const Value& defaultValue(ClassId cls, const PropertyInfo& prop) {
    static const DefaultCache cache;
    const ClassInfo& c = classInfo(cls);
    const size_t index = static_cast<size_t>(&prop - c.props.data());
    static const Value none;
    return index < cache.values[static_cast<int>(cls)].size() ? cache.values[static_cast<int>(cls)][index] : none;
}

Value getProperty(const Instance& instance, const PropertyInfo& prop) { return prop.get(instance); }

Value coerce(const PropertyInfo& prop, const Value& value) {
    const ValueType have = typeOf(value);
    if (have == prop.type) return value;
    switch (prop.type) {
        case ValueType::Float:
            if (have == ValueType::Int) return static_cast<float>(std::get<int>(value));
            break;
        case ValueType::Int:
            if (have == ValueType::Float) return static_cast<int>(std::lround(std::get<float>(value)));
            break;
        case ValueType::Enum:
            if (have == ValueType::Int) return EnumValue{ std::get<int>(value) };
            if (have == ValueType::Float) return EnumValue{ static_cast<int>(std::get<float>(value)) };
            if (have == ValueType::String) {
                const auto& names = prop.enumNames;
                auto it = std::find(names.begin(), names.end(), std::get<std::string>(value));
                if (it != names.end()) return EnumValue{ static_cast<int>(it - names.begin()) };
            }
            break;
        case ValueType::Asset:
            if (have == ValueType::String) return AssetPath{ std::get<std::string>(value) };
            break;
        case ValueType::String:
            if (have == ValueType::Asset) return std::get<AssetPath>(value).path;
            break;
        case ValueType::Color3:
            if (have == ValueType::Vector3) return Color3::from(std::get<glm::vec3>(value));
            break;
        default:
            break;
    }
    return {};
}

bool setProperty(Instance& instance, const PropertyInfo& prop, const Value& value, std::string* error) {
    Value v = coerce(prop, value);
    if (typeOf(v) == ValueType::None) {
        if (error) *error = prop.name + " expects " + typeName(prop.type) + ", got " + typeName(typeOf(value));
        return false;
    }
    // Clamp to the property's range; reject non-finite numbers outright.
    if (prop.type == ValueType::Float) {
        float f = std::get<float>(v);
        if (!std::isfinite(f)) { if (error) *error = prop.name + " must be a finite number"; return false; }
        if (prop.bounded()) f = std::clamp(f, prop.minValue, prop.maxValue);
        v = f;
    } else if (prop.type == ValueType::Int && prop.bounded()) {
        v = std::clamp(std::get<int>(v), static_cast<int>(prop.minValue), static_cast<int>(prop.maxValue));
    } else if (prop.type == ValueType::Vector3) {
        glm::vec3 x = std::get<glm::vec3>(v);
        if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z)) { if (error) *error = prop.name + " must be finite"; return false; }
        if (prop.bounded()) x = glm::clamp(x, glm::vec3(prop.minValue), glm::vec3(prop.maxValue));
        v = x;
    } else if (prop.type == ValueType::Vector2 && prop.bounded()) {
        v = glm::clamp(std::get<glm::vec2>(v), glm::vec2(prop.minValue), glm::vec2(prop.maxValue));
    } else if (prop.type == ValueType::Color3) {
        const Color3 c = std::get<Color3>(v);
        v = Color3(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f));
    } else if (prop.type == ValueType::NumberRange) {
        NumberRange r = std::get<NumberRange>(v);
        if (!std::isfinite(r.min) || !std::isfinite(r.max)) { if (error) *error = prop.name + " must be finite"; return false; }
        if (r.max < r.min) std::swap(r.min, r.max);
        if (prop.bounded()) r = NumberRange(std::clamp(r.min, prop.minValue, prop.maxValue), std::clamp(r.max, prop.minValue, prop.maxValue));
        v = r;
    } else if (prop.type == ValueType::NumberSequence) {
        NumberSequence s = std::get<NumberSequence>(v);
        if (s.keys.size() > 20) s.keys.resize(20);
        for (NumberKeypoint& k : s.keys) {
            if (!std::isfinite(k.value) || !std::isfinite(k.envelope) || !std::isfinite(k.time)) { if (error) *error = prop.name + " must be finite"; return false; }
            if (prop.bounded()) k.value = std::clamp(k.value, prop.minValue, prop.maxValue);
            k.envelope = std::max(k.envelope, 0.0f);
        }
        v = NumberSequence(std::move(s.keys));
    } else if (prop.type == ValueType::ColorSequence) {
        ColorSequence s = std::get<ColorSequence>(v);
        if (s.keys.size() > 20) s.keys.resize(20);
        for (ColorKeypoint& k : s.keys) k.color = Color3(std::clamp(k.color.r, 0.0f, 1.0f), std::clamp(k.color.g, 0.0f, 1.0f), std::clamp(k.color.b, 0.0f, 1.0f));
        v = ColorSequence(std::move(s.keys));
    } else if (prop.type == ValueType::Enum) {
        const int index = std::get<EnumValue>(v).index;
        if (index < 0 || index >= static_cast<int>(prop.enumNames.size())) {
            if (error) *error = prop.name + ": invalid value " + std::to_string(index);
            return false;
        }
    }
    if (!prop.set(instance, v)) {
        if (error) *error = prop.name + " cannot be changed on " + classInfo(instance.cls).name;
        return false;
    }
    return true;
}

const std::vector<std::pair<std::string, std::vector<std::string>>>& allEnums() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
        { "PartType", kPartTypes },
        { "Material", kMaterials },
        { "CameraMode", kCameraModes },
        { "TextXAlignment", kTextX },
        { "TextYAlignment", kTextY },
        { "ScaleType", kScaleTypes },
        { "Font", kFonts },
        { "NormalId", kNormalIds },
        { "ParticleOrientation", kParticleOrientations },
        { "TextureMode", kTextureModes },
        { "AnimationPriority", kAnimationPriorities },
        { "PoseEasingStyle", kPoseEasingStyles },
        { "PoseEasingDirection", kPoseEasingDirections },
    };
    return table;
}

const std::vector<std::string>& enumNamesFor(const std::string& enumType) {
    for (const auto& [name, names] : allEnums()) if (name == enumType) return names;
    static const std::vector<std::string> none;
    return none;
}

} // namespace rift
