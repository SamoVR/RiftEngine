#include "scene/Reflection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace rift {

namespace {

// ---------------------------------------------------------------- enum tables
const std::vector<std::string> kPartTypes = { "Block", "Ball", "Cylinder", "Wedge", "CornerWedge" };
const std::vector<std::string> kMaterials = { "Plastic", "SmoothPlastic", "Metal", "Neon", "Glass", "Wood", "Concrete" };
const std::vector<std::string> kCameraModes = { "ThirdPerson", "FirstPerson" };
const std::vector<std::string> kTextX = { "Left", "Center", "Right" };
const std::vector<std::string> kTextY = { "Top", "Center", "Bottom" };
const std::vector<std::string> kScaleTypes = { "Stretch", "Fit", "Crop" };

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
    };
    return table;
}

const std::vector<std::string>& enumNamesFor(const std::string& enumType) {
    for (const auto& [name, names] : allEnums()) if (name == enumType) return names;
    static const std::vector<std::string> none;
    return none;
}

} // namespace rift
