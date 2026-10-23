#pragma once
//
// Value.h -- the dynamically typed value of a reflected property or attribute.
//
// Properties (Instance fields exposed to the editor, scene files and Lua) and
// user attributes are all read and written as `Value`, so one code path
// handles widgets, JSON, undo and scripting for every type.
//
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <string>
#include <variant>

namespace rift {

struct Color3 {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    Color3() = default;
    constexpr Color3(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
    static Color3 fromRGB(int r, int g, int b) { return { r / 255.0f, g / 255.0f, b / 255.0f }; }
    glm::vec3 vec() const { return { r, g, b }; }
    static Color3 from(const glm::vec3& v) { return { v.x, v.y, v.z }; }
    bool operator==(const Color3& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color3& o) const { return !(*this == o); }
};

// GUI coordinates: a fraction of the parent's size plus a pixel offset (Roblox UDim2).
struct UDim2 {
    float xScale = 0.0f, xOffset = 0.0f, yScale = 0.0f, yOffset = 0.0f;
    bool operator==(const UDim2& o) const { return xScale == o.xScale && xOffset == o.xOffset && yScale == o.yScale && yOffset == o.yOffset; }
    bool operator!=(const UDim2& o) const { return !(*this == o); }
};

// An enum property's value: its index into the property's name table.
struct EnumValue {
    int index = 0;
    bool operator==(const EnumValue& o) const { return index == o.index; }
    bool operator!=(const EnumValue& o) const { return !(*this == o); }
};

// A reference to an asset file, stored portably ("assets/x.png", "engine:...").
struct AssetPath {
    std::string path;
    bool operator==(const AssetPath& o) const { return path == o.path; }
    bool operator!=(const AssetPath& o) const { return !(*this == o); }
};

enum class ValueType : uint8_t { None, Bool, Int, Float, String, Vector3, Vector2, Color3, UDim2, Enum, Asset };

using Value = std::variant<std::monostate, bool, int, float, std::string, glm::vec3, glm::vec2, Color3, UDim2, EnumValue, AssetPath>;

inline ValueType typeOf(const Value& v) { return static_cast<ValueType>(v.index()); }
const char* typeName(ValueType t);

// Exact comparison (used for "is this the default?" and undo no-op detection).
bool valuesEqual(const Value& a, const Value& b);

// JSON encoding used by scene files. `type` tells the decoder what to expect,
// because JSON cannot distinguish e.g. a Vector3 from a Color3.
nlohmann::json toJson(const Value& v);
// Returns monostate if the JSON does not fit `type`.
Value fromJson(const nlohmann::json& j, ValueType type);
// Attributes store their type next to the value: {"t":"Vector3","v":[1,2,3]}.
nlohmann::json toTaggedJson(const Value& v);
Value fromTaggedJson(const nlohmann::json& j);
ValueType valueTypeFromName(const std::string& name);

// Human-readable text (Output panel, tooltips): "1, 2, 3", "true", ...
std::string toDisplayString(const Value& v);

} // namespace rift
