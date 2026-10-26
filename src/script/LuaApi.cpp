// LuaApi.cpp -- the Lua API as data (see LuaApi.h). The rules here decide
// what instances offer at run time; the lists feed the script editor's
// autocomplete and are checked against the runtime by the tests.
#include "script/LuaApi.h"
#include "scene/Reflection.h"

#include <algorithm>
#include <unordered_map>

namespace rift::luaapi {

// Roblox's abstract base classes, for IsA / FindFirstChildWhichIsA.
bool isA(const Instance& i, const std::string& cls) {
    if (cls == "Instance") return true;
    if (cls == classInfo(i.cls).name) return true;
    if (cls == "BasePart") return i.isBasePart();
    if (cls == "PVInstance") return i.hasTransform() || i.cls == ClassId::Workspace;
    if (cls == "Model") return i.cls == ClassId::Model || i.cls == ClassId::Workspace;
    if (cls == "LuaSourceContainer") return i.isScript();
    if (cls == "BaseScript") return i.cls == ClassId::Script || i.cls == ClassId::LocalScript;
    if (cls == "GuiObject") return i.isGuiObject();
    if (cls == "GuiButton") return i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton;
    if (cls == "GuiBase2d") return i.isGui();
    if (cls == "LayerCollector") return i.cls == ClassId::ScreenGui;
    if (cls == "UIComponent" || cls == "UIBase") return i.cls == ClassId::UIGradient;
    if (cls == "Light") return i.isLight();
    if (cls == "ValueBase") return i.isValueObject();
    return false;
}

bool hasEvent(const Instance& i, const std::string& name) {
    static const char* common[] = { "Changed", "ChildAdded", "ChildRemoved", "DescendantAdded", "DescendantRemoving",
                                    "AncestryChanged", "Destroying", "AttributeChanged" };
    for (const char* e : common) if (name == e) return true;
    if (i.isBasePart()) return name == "Touched" || name == "TouchEnded";
    if (i.cls == ClassId::Humanoid) {
        return name == "Died" || name == "HealthChanged" || name == "Jumping" || name == "Running" || name == "StateChanged"
            || name == "FreeFalling" || name == "Swimming" || name == "AnimationPlayed";
    }
    if (i.cls == ClassId::Animator) return name == "AnimationPlayed";
    if (i.cls == ClassId::Sound) return name == "Ended";
    if (i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton) {
        return name == "Activated" || name == "MouseButton1Click" || name == "MouseButton1Down" || name == "MouseButton1Up"
            || name == "MouseEnter" || name == "MouseLeave";
    }
    if (i.isGuiObject()) return name == "MouseEnter" || name == "MouseLeave";
    if (i.cls == ClassId::Player) return name == "CharacterAdded" || name == "CharacterRemoving";
    if (i.cls == ClassId::RemoteEvent) return name == "OnServerEvent" || name == "OnClientEvent";
    if (i.cls == ClassId::BindableEvent) return name == "Event";
    if (i.cls == ClassId::Tool) return name == "Equipped" || name == "Unequipped" || name == "Activated" || name == "Deactivated";
    return false;
}

// Methods that only exist on some classes. Anything else is shared by all
// instances. On other classes the name falls through to child lookup, so a
// button named "Play" is found even though Sound has a :Play() method.
bool methodApplies(const std::string& name, const Instance& i) {
    static const std::unordered_map<std::string, bool (*)(const Instance&)> only = {
        { "Play", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Stop", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Pause", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Resume", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "LoadCharacter", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "Kick", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "GetMouse", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "GetPlayers", [](const Instance& x) { return x.cls == ClassId::Players; } },
        { "GetPlayerFromCharacter", [](const Instance& x) { return x.cls == ClassId::Players; } },
        { "Raycast", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "GetPartBoundsInBox", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "GetPartsInPart", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "TakeDamage", [](const Instance& x) { return x.cls == ClassId::Humanoid; } },
        { "GetTouchingParts", [](const Instance& x) { return x.isBasePart(); } },
        { "ApplyImpulse", [](const Instance& x) { return x.isBasePart(); } },
        { "ApplyAngularImpulse", [](const Instance& x) { return x.isBasePart(); } },
        { "GetMass", [](const Instance& x) { return x.isBasePart(); } },
        { "GetPivot", [](const Instance& x) { return x.hasTransform(); } },
        { "PivotTo", [](const Instance& x) { return x.hasTransform(); } },
        { "SetPrimaryPartCFrame", [](const Instance& x) { return x.cls == ClassId::Model; } },
        { "MoveTo", [](const Instance& x) { return x.hasTransform() || x.cls == ClassId::Humanoid; } },
        { "GetBoundingBox", [](const Instance& x) { return x.cls == ClassId::Model || x.isBasePart(); } },
        { "GetExtentsSize", [](const Instance& x) { return x.cls == ClassId::Model; } },
        { "FireServer", [](const Instance& x) { return x.cls == ClassId::RemoteEvent; } },
        { "FireClient", [](const Instance& x) { return x.cls == ClassId::RemoteEvent; } },
        { "FireAllClients", [](const Instance& x) { return x.cls == ClassId::RemoteEvent; } },
        { "InvokeServer", [](const Instance& x) { return x.cls == ClassId::RemoteFunction; } },
        { "InvokeClient", [](const Instance& x) { return x.cls == ClassId::RemoteFunction; } },
        { "Fire", [](const Instance& x) { return x.cls == ClassId::BindableEvent; } },
        { "Invoke", [](const Instance& x) { return x.cls == ClassId::BindableFunction; } },
        { "Activate", [](const Instance& x) { return x.cls == ClassId::Tool; } },
        { "LoadAnimation", [](const Instance& x) { return x.cls == ClassId::Animator || x.cls == ClassId::Humanoid || x.cls == ClassId::AnimationController; } },
        { "GetPlayingAnimationTracks", [](const Instance& x) { return x.cls == ClassId::Animator || x.cls == ClassId::Humanoid || x.cls == ClassId::AnimationController; } },
        { "GetState", [](const Instance& x) { return x.cls == ClassId::Humanoid; } },
        { "Emit", [](const Instance& x) { return x.cls == ClassId::ParticleEmitter; } },
        { "FillBlock", [](const Instance& x) { return x.cls == ClassId::Terrain; } },
        { "FillBall", [](const Instance& x) { return x.cls == ClassId::Terrain; } },
        { "FillCylinder", [](const Instance& x) { return x.cls == ClassId::Terrain; } },
        { "Clear", [](const Instance& x) { return x.cls == ClassId::Terrain || x.cls == ClassId::ParticleEmitter; } },
        { "GetMaterialColor", [](const Instance& x) { return x.cls == ClassId::Terrain; } },
        { "SetMaterialColor", [](const Instance& x) { return x.cls == ClassId::Terrain; } },
        { "Deactivate", [](const Instance& x) { return x.cls == ClassId::Tool; } },
        { "EquipTool", [](const Instance& x) { return x.cls == ClassId::Humanoid; } },
        { "SetCoreGuiEnabled", [](const Instance& x) { return x.cls == ClassId::StarterGui; } },
        { "GetCoreGuiEnabled", [](const Instance& x) { return x.cls == ClassId::StarterGui; } },
        { "UnequipTools", [](const Instance& x) { return x.cls == ClassId::Humanoid; } },
    };
    auto it = only.find(name);
    return it == only.end() || it->second(i);
}


// ============================================================================
//  Catalogue
// ============================================================================
namespace {
    std::vector<std::string> sorted(std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
}

const std::vector<std::string>& methodNames() {
    static const std::vector<std::string> list = sorted({
        "Activate", "AddTag", "ApplyAngularImpulse", "ApplyImpulse", "Clear", "ClearAllChildren", "Clone", "Deactivate", "Destroy", "Emit",
        "EquipTool", "FillBall", "FillBlock", "FillCylinder", "FindFirstAncestor", "FindFirstAncestorOfClass", "FindFirstAncestorWhichIsA",
        "FindFirstChild", "FindFirstChildOfClass", "FindFirstChildWhichIsA", "FindFirstDescendant", "Fire", "FireAllClients", "FireClient",
        "FireServer", "GetAttribute", "GetAttributeChangedSignal", "GetAttributes", "GetBoundingBox", "GetChildren", "GetCoreGuiEnabled",
        "GetDescendants", "GetExtentsSize", "GetFullName", "GetMass", "GetMaterialColor", "GetMouse", "GetPartBoundsInBox", "GetPartsInPart",
        "GetPivot", "GetPlayerFromCharacter", "GetPlayers", "GetPlayingAnimationTracks", "GetPropertyChangedSignal", "GetState", "GetTags",
        "GetTouchingParts", "HasTag", "Invoke", "InvokeClient", "InvokeServer", "IsA", "IsAncestorOf", "IsDescendantOf", "Kick", "LoadAnimation",
        "LoadCharacter", "MoveTo", "Pause", "PivotTo", "Play", "Raycast", "Remove", "RemoveTag", "Resume", "SetAttribute", "SetCoreGuiEnabled",
        "SetMaterialColor", "SetPrimaryPartCFrame", "Stop", "TakeDamage", "UnequipTools", "WaitForChild",
    });
    return list;
}

const std::vector<std::string>& eventNames() {
    static const std::vector<std::string> list = sorted({
        "Changed", "ChildAdded", "ChildRemoved", "DescendantAdded", "DescendantRemoving", "AncestryChanged", "Destroying", "AttributeChanged",
        "Touched", "TouchEnded", "Died", "HealthChanged", "Jumping", "Running", "StateChanged", "FreeFalling", "Swimming", "AnimationPlayed",
        "Ended", "Activated", "MouseButton1Click", "MouseButton1Down", "MouseButton1Up", "MouseEnter", "MouseLeave", "CharacterAdded",
        "CharacterRemoving", "OnServerEvent", "OnClientEvent", "Event", "Equipped", "Unequipped", "Deactivated",
    });
    return list;
}

std::vector<std::string> instanceMembers(ClassId cls, bool methods) {
    Instance probe;
    probe.cls = cls;
    std::vector<std::string> out;
    if (methods) {
        for (const std::string& m : methodNames()) if (methodApplies(m, probe)) out.push_back(m);
        return out;
    }
    for (const PropertyInfo& p : classInfo(cls).props) if (p.has(PF_ScriptRead)) out.push_back(p.name);
    out.insert(out.end(), { "Parent", "ClassName" });
    if (probe.hasTransform()) out.insert(out.end(), { "CFrame", "Orientation" });
    if (probe.isBasePart()) out.insert(out.end(), { "AssemblyLinearVelocity", "AssemblyAngularVelocity", "BrickColor" });
    if (cls == ClassId::Model) out.push_back("PrimaryPart");
    if (cls == ClassId::Player) out.insert(out.end(), { "Character", "UserId", "DisplayName" });
    if (cls == ClassId::Humanoid) out.push_back("RootPart");
    if (cls == ClassId::Tool) out.push_back("Grip");
    if (cls == ClassId::Players) out.insert(out.end(), { "LocalPlayer", "PlayerAdded" });
    if (cls == ClassId::Sound) out.insert(out.end(), { "IsPlaying", "TimePosition", "TimeLength" });
    if (cls == ClassId::Workspace) out.insert(out.end(), { "CurrentCamera", "Terrain" });
    for (const std::string& e : eventNames()) if (hasEvent(probe, e)) out.push_back(e);
    return sorted(std::move(out));
}

const std::vector<std::string>& globals() {
    static const std::vector<std::string> list = sorted({
        "game", "workspace", "script", "shared", "print", "warn", "error", "assert", "typeof", "type", "require", "wait", "spawn", "delay",
        "tick", "time", "elapsedTime", "pairs", "ipairs", "next", "select", "tostring", "tonumber", "pcall", "xpcall", "unpack", "rawget",
        "rawset", "rawequal", "setmetatable", "getmetatable",
        "math", "string", "table", "task", "coroutine", "os", "utf8",
        "Instance", "Vector3", "Vector2", "CFrame", "Color3", "BrickColor", "UDim", "UDim2", "Enum", "TweenInfo", "Random", "RaycastParams",
        "NumberRange", "NumberSequence", "NumberSequenceKeypoint", "ColorSequence", "ColorSequenceKeypoint",
    });
    return list;
}

const std::vector<std::string>& keywords() {
    static const std::vector<std::string> list = sorted({
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not", "or", "repeat",
        "return", "then", "true", "until", "while", "continue",
    });
    return list;
}

const std::vector<std::string>& libraryMembers(const std::string& library) {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        { "math", sorted({ "abs", "acos", "asin", "atan", "ceil", "clamp", "cos", "deg", "exp", "floor", "fmod", "huge", "lerp", "log", "map",
                           "max", "min", "modf", "noise", "pi", "rad", "random", "randomseed", "round", "sign", "sin", "sqrt", "tan", "tointeger",
                           "type", "ult" }) },
        { "string", sorted({ "byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", "match", "rep", "reverse", "split", "sub", "upper" }) },
        { "table", sorted({ "clear", "clone", "concat", "find", "freeze", "insert", "isfrozen", "move", "pack", "remove", "sort", "unpack" }) },
        { "task", sorted({ "cancel", "defer", "delay", "spawn", "wait" }) },
        { "coroutine", sorted({ "close", "create", "isyieldable", "resume", "running", "status", "wrap", "yield" }) },
        { "os", sorted({ "clock", "date", "difftime", "time" }) },
        { "utf8", sorted({ "char", "charpattern", "codepoint", "codes", "len", "offset" }) },
    };
    static const std::vector<std::string> none;
    auto it = table.find(library);
    return it == table.end() ? none : it->second;
}

const std::vector<std::string>& typeStatics(const std::string& type) {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        { "Vector3", { "new", "one", "xAxis", "yAxis", "zAxis", "zero" } },
        { "Vector2", { "new", "one", "zero" } },
        { "CFrame", sorted({ "new", "lookAt", "Angles", "fromEulerAnglesXYZ", "fromEulerAnglesYXZ", "fromOrientation", "fromAxisAngle", "identity" }) },
        { "Color3", { "fromHSV", "fromHex", "fromRGB", "new" } },
        { "BrickColor", sorted({ "new", "Random", "White", "Gray", "DarkGray", "Black", "Red", "Yellow", "Green", "Blue" }) },
        { "UDim", { "new" } },
        { "UDim2", { "fromOffset", "fromScale", "new" } },
        { "Instance", { "new" } },
        { "TweenInfo", { "new" } },
        { "Random", { "new" } },
        { "RaycastParams", { "new" } },
        { "NumberRange", { "new" } },
        { "NumberSequence", { "new" } },
        { "NumberSequenceKeypoint", { "new" } },
        { "ColorSequence", { "new" } },
        { "ColorSequenceKeypoint", { "new" } },
    };
    static const std::vector<std::string> none;
    auto it = table.find(type);
    return it == table.end() ? none : it->second;
}

const std::vector<std::string>& valueMembers(const std::string& type) {
    static const std::unordered_map<std::string, std::vector<std::string>> table = {
        { "Vector3", sorted({ "X", "Y", "Z", "Magnitude", "Unit", "Abs", "Angle", "Ceil", "Cross", "Dot", "Floor", "FuzzyEq", "Lerp", "Max", "Min", "Sign" }) },
        { "Vector2", sorted({ "X", "Y", "Magnitude", "Unit", "Dot", "Lerp" }) },
        { "CFrame", sorted({ "Position", "X", "Y", "Z", "LookVector", "RightVector", "UpVector", "XVector", "YVector", "ZVector", "Rotation",
                             "Inverse", "Lerp", "ToWorldSpace", "ToObjectSpace", "PointToWorldSpace", "PointToObjectSpace", "VectorToWorldSpace",
                             "VectorToObjectSpace", "ToEulerAnglesXYZ", "ToOrientation", "GetComponents" }) },
        { "Color3", sorted({ "R", "G", "B", "Lerp", "ToHSV", "ToHex" }) },
        { "BrickColor", sorted({ "Color", "Name", "Number", "r", "g", "b" }) },
        { "UDim2", sorted({ "X", "Y", "Width", "Height", "Lerp" }) },
        { "UDim", { "Offset", "Scale" } },
        { "RBXScriptSignal", { "Connect", "Once", "Wait" } },
        { "RBXScriptConnection", { "Connected", "Disconnect" } },
        { "AnimationTrack", sorted({ "AdjustSpeed", "AdjustWeight", "Animation", "DidLoop", "Ended", "GetMarkerReachedSignal", "GetTimeOfKeyframe",
                                     "IsPlaying", "KeyframeReached", "Length", "Looped", "Name", "Play", "Priority", "Speed", "Stop", "Stopped",
                                     "TimePosition", "WeightCurrent", "WeightTarget" }) },
        { "Tween", sorted({ "Cancel", "Completed", "Pause", "Play", "PlaybackState" }) },
        { "EnumItem", { "EnumType", "IsA", "Name", "Value" } },
        { "Mouse", sorted({ "Button1Down", "Button1Up", "Button2Down", "Button2Up", "Hit", "Move", "Target", "UnitRay", "ViewSizeX", "ViewSizeY",
                            "WheelBackward", "WheelForward", "X", "Y" }) },
        { "InputObject", sorted({ "Delta", "KeyCode", "Position", "UserInputState", "UserInputType" }) },
        { "RaycastResult", sorted({ "Distance", "Instance", "Material", "Normal", "Position" }) },
        { "NumberRange", { "Max", "Min" } },
        { "NumberSequence", { "Keypoints" } },
        { "ColorSequence", { "Keypoints" } },
        { "Random", sorted({ "NextInteger", "NextNumber", "NextUnitVector" }) },
        // Services that are not objects in the Explorer.
        { "RunService", sorted({ "BindToRenderStep", "Heartbeat", "IsClient", "IsEdit", "IsRunning", "IsServer", "IsStudio", "RenderStepped",
                                 "Stepped", "UnbindFromRenderStep" }) },
        { "UserInputService", sorted({ "GamepadEnabled", "GetKeysPressed", "GetMouseDelta", "GetMouseLocation", "InputBegan", "InputChanged",
                                       "InputEnded", "IsKeyDown", "IsMouseButtonPressed", "KeyboardEnabled", "MouseBehavior", "MouseEnabled",
                                       "MouseIconEnabled", "TouchEnabled" }) },
        { "TweenService", { "Create" } },
        { "Debris", { "AddItem" } },
        { "CollectionService", sorted({ "AddTag", "GetTagged", "GetTags", "HasTag", "RemoveTag" }) },
        { "HttpService", sorted({ "GenerateGUID", "JSONDecode", "JSONEncode" }) },
        { "DataStoreService", { "GetDataStore" } },
        { "DataStore", sorted({ "GetAsync", "IncrementAsync", "SetAsync", "UpdateAsync" }) },
        { "KeyframeSequenceProvider", sorted({ "GetKeyframeSequenceAsync", "RegisterKeyframeSequence" }) },
    };
    static const std::vector<std::string> none;
    auto it = table.find(type);
    return it == table.end() ? none : it->second;
}

const std::vector<std::string>& serviceNames() {
    static const std::vector<std::string> list = sorted({
        "Workspace", "Players", "Lighting", "ReplicatedStorage", "ServerStorage", "ServerScriptService", "ScriptService", "StarterGui",
        "StarterPack", "StarterPlayer", "SoundService", "RunService", "UserInputService", "TweenService", "Debris", "CollectionService",
        "HttpService", "DataStoreService", "KeyframeSequenceProvider",
    });
    return list;
}

} // namespace rift::luaapi
