// LuaTypes.cpp -- value types exposed to scripts and Value <-> Lua conversion.
#include "script/LuaInternal.h"
#include "platform/Input.h"
#include "core/TransformUtil.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rift {

using namespace rift::lua;

namespace lua {

// ============================================================================
//  Enums
// ============================================================================
const std::pair<std::string, int>* EnumType::byName(const std::string& n) const {
    for (const auto& item : items) if (item.first == n) return &item;
    return nullptr;
}

const std::pair<std::string, int>* EnumType::byValue(int v) const {
    for (const auto& item : items) if (item.second == v) return &item;
    return nullptr;
}

std::string EnumItem::name() const {
    if (!type) return "?";
    const auto* item = type->byValue(value);
    return item ? item->first : std::to_string(value);
}

namespace {
    std::vector<EnumType> buildEnums() {
        std::vector<EnumType> out;
        auto add = [&](const std::string& name, const std::vector<std::string>& names) {
            EnumType t;
            t.name = name;
            for (size_t i = 0; i < names.size(); ++i) t.items.emplace_back(names[i], static_cast<int>(i));
            out.push_back(std::move(t));
        };
        for (const auto& [name, names] : allEnums()) add(name, names);
        {
            EnumType keys;
            keys.name = "KeyCode";
            keys.items = keyCodeTable();
            keys.items.emplace_back("Unknown", 0);
            out.push_back(std::move(keys));
        }
        add("UserInputType", { "None", "Keyboard", "MouseButton1", "MouseButton2", "MouseButton3", "MouseMovement", "MouseWheel" });
        add("UserInputState", { "Begin", "Change", "End", "Cancel", "None" });
        add("EasingStyle", { "Linear", "Sine", "Quad", "Cubic", "Quart", "Quint", "Exponential", "Circular", "Back", "Bounce", "Elastic" });
        add("EasingDirection", { "In", "Out", "InOut" });
        {
            EnumType t;
            t.name = "RaycastFilterType";
            t.items = { { "Exclude", 0 }, { "Include", 1 }, { "Blacklist", 0 }, { "Whitelist", 1 } };
            out.push_back(std::move(t));
        }
        add("MouseBehavior", { "Default", "LockCenter", "LockCurrentPosition" });
        add("CameraType", { "Custom", "Scriptable", "Fixed" });
        add("CoreGuiType", { "PlayerList", "Health", "Backpack", "Chat", "All", "EmotesMenu" });
        add("PlaybackState", { "Begin", "Playing", "Paused", "Completed", "Cancelled", "Delayed" });
        return out;
    }
    const std::vector<EnumType>& enumStorage() {
        static const std::vector<EnumType> storage = buildEnums();
        return storage;
    }
}

const EnumType* enumType(const std::string& name) {
    for (const EnumType& t : enumStorage()) if (t.name == name) return &t;
    return nullptr;
}

const std::vector<const EnumType*>& allEnumTypes() {
    static const std::vector<const EnumType*> list = [] {
        std::vector<const EnumType*> l;
        for (const EnumType& t : enumStorage()) l.push_back(&t);
        return l;
    }();
    return list;
}

// ============================================================================
//  CFrame helpers
// ============================================================================
CFrame CFrame::fromMat4(const glm::mat4& m) {
    CFrame c;
    c.p = glm::vec3(m[3]);
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) { const float l = glm::length(r[i]); if (l > 1e-8f) r[i] /= l; }
    c.r = r;
    return c;
}

} // namespace lua

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

CFrame lookAtFrame(const glm::vec3& pos, const glm::vec3& target, glm::vec3 up) {
    CFrame c;
    c.p = pos;
    glm::vec3 forward = target - pos;
    if (glm::length(forward) < 1e-8f) return c;
    forward = glm::normalize(forward);
    if (std::abs(glm::dot(forward, glm::normalize(up))) > 0.9999f) up = std::abs(forward.y) > 0.9f ? glm::vec3(0, 0, -1) : glm::vec3(0, 1, 0);
    const glm::vec3 right = glm::normalize(glm::cross(forward, up));
    const glm::vec3 realUp = glm::cross(right, forward);
    c.r = glm::mat3(right, realUp, -forward); // columns: X, Y, Z (looks along -Z)
    return c;
}

CFrame mul(const CFrame& a, const CFrame& b) {
    CFrame c;
    c.r = a.r * b.r;
    c.p = a.p + a.r * b.p;
    return c;
}

CFrame inverse(const CFrame& a) {
    CFrame c;
    c.r = glm::transpose(a.r);
    c.p = -(c.r * a.p);
    return c;
}

CFrame lerp(const CFrame& a, const CFrame& b, float t) {
    CFrame c;
    c.p = glm::mix(a.p, b.p, t);
    c.r = glm::mat3_cast(glm::slerp(glm::quat_cast(a.r), glm::quat_cast(b.r), t));
    return c;
}

// Rx * Ry * Rz, radians (same order as part Rotation and Roblox CFrame.Angles).
glm::mat3 anglesXYZ(float x, float y, float z) {
    return glm::mat3(TransformUtil::compose(glm::vec3(0.0f), glm::degrees(glm::vec3(x, y, z)), glm::vec3(1.0f)));
}

glm::vec3 toEulerXYZ(const glm::mat3& r) {
    glm::vec3 p, deg, s;
    TransformUtil::decompose(glm::mat4(r), p, deg, s);
    return glm::radians(deg);
}

} // namespace

// ============================================================================
//  Registration
// ============================================================================
void LuaRuntime::Impl::registerTypes() {
    // ------------------------------------------------------------ Vector3
    lua.new_usertype<Vector3L>("Vector3",
        "new", sol::factories([](sol::optional<float> x, sol::optional<float> y, sol::optional<float> z) {
            return Vector3L{ { x.value_or(0.0f), y.value_or(0.0f), z.value_or(0.0f) } };
        }),
        "X", sol::property([](const Vector3L& a) { return a.v.x; }),
        "Y", sol::property([](const Vector3L& a) { return a.v.y; }),
        "Z", sol::property([](const Vector3L& a) { return a.v.z; }),
        "Magnitude", sol::property([](const Vector3L& a) { return glm::length(a.v); }),
        "Unit", sol::property([](const Vector3L& a) { const float l = glm::length(a.v); return Vector3L{ l > 1e-8f ? a.v / l : glm::vec3(0.0f) }; }),
        "Dot", [](const Vector3L& a, const Vector3L& b) { return glm::dot(a.v, b.v); },
        "Cross", [](const Vector3L& a, const Vector3L& b) { return Vector3L{ glm::cross(a.v, b.v) }; },
        "Lerp", [](const Vector3L& a, const Vector3L& b, float t) { return Vector3L{ glm::mix(a.v, b.v, t) }; },
        "Abs", [](const Vector3L& a) { return Vector3L{ glm::abs(a.v) }; },
        "Floor", [](const Vector3L& a) { return Vector3L{ glm::floor(a.v) }; },
        "Ceil", [](const Vector3L& a) { return Vector3L{ glm::ceil(a.v) }; },
        "Sign", [](const Vector3L& a) { return Vector3L{ glm::sign(a.v) }; },
        "Min", [](const Vector3L& a, const Vector3L& b) { return Vector3L{ glm::min(a.v, b.v) }; },
        "Max", [](const Vector3L& a, const Vector3L& b) { return Vector3L{ glm::max(a.v, b.v) }; },
        "Angle", [](const Vector3L& a, const Vector3L& b) {
            const float la = glm::length(a.v), lb = glm::length(b.v);
            if (la < 1e-8f || lb < 1e-8f) return 0.0f;
            return std::acos(std::clamp(glm::dot(a.v, b.v) / (la * lb), -1.0f, 1.0f));
        },
        "FuzzyEq", [](const Vector3L& a, const Vector3L& b, sol::optional<float> eps) { return glm::length(a.v - b.v) <= eps.value_or(1e-5f); },
        "zero", sol::var(Vector3L{ glm::vec3(0.0f) }),
        "one", sol::var(Vector3L{ glm::vec3(1.0f) }),
        "xAxis", sol::var(Vector3L{ { 1, 0, 0 } }),
        "yAxis", sol::var(Vector3L{ { 0, 1, 0 } }),
        "zAxis", sol::var(Vector3L{ { 0, 0, 1 } }),
        sol::meta_function::addition, [](const Vector3L& a, const Vector3L& b) { return Vector3L{ a.v + b.v }; },
        sol::meta_function::subtraction, [](const Vector3L& a, const Vector3L& b) { return Vector3L{ a.v - b.v }; },
        sol::meta_function::multiplication, sol::overload(
            [](const Vector3L& a, const Vector3L& b) { return Vector3L{ a.v * b.v }; },
            [](const Vector3L& a, float s) { return Vector3L{ a.v * s }; },
            [](float s, const Vector3L& a) { return Vector3L{ a.v * s }; }),
        sol::meta_function::division, sol::overload(
            [](const Vector3L& a, const Vector3L& b) { return Vector3L{ a.v / b.v }; },
            [](const Vector3L& a, float s) { return Vector3L{ a.v / s }; }),
        sol::meta_function::unary_minus, [](const Vector3L& a) { return Vector3L{ -a.v }; },
        sol::meta_function::equal_to, [](const Vector3L& a, const Vector3L& b) { return a.v == b.v; },
        sol::meta_function::to_string, [](const Vector3L& a) { return num(a.v.x) + ", " + num(a.v.y) + ", " + num(a.v.z); });

    // ------------------------------------------------------------ Vector2
    lua.new_usertype<Vector2L>("Vector2",
        "new", sol::factories([](sol::optional<float> x, sol::optional<float> y) { return Vector2L{ { x.value_or(0.0f), y.value_or(0.0f) } }; }),
        "X", sol::property([](const Vector2L& a) { return a.v.x; }),
        "Y", sol::property([](const Vector2L& a) { return a.v.y; }),
        "Magnitude", sol::property([](const Vector2L& a) { return glm::length(a.v); }),
        "Unit", sol::property([](const Vector2L& a) { const float l = glm::length(a.v); return Vector2L{ l > 1e-8f ? a.v / l : glm::vec2(0.0f) }; }),
        "Dot", [](const Vector2L& a, const Vector2L& b) { return glm::dot(a.v, b.v); },
        "Lerp", [](const Vector2L& a, const Vector2L& b, float t) { return Vector2L{ glm::mix(a.v, b.v, t) }; },
        "zero", sol::var(Vector2L{ glm::vec2(0.0f) }),
        "one", sol::var(Vector2L{ glm::vec2(1.0f) }),
        sol::meta_function::addition, [](const Vector2L& a, const Vector2L& b) { return Vector2L{ a.v + b.v }; },
        sol::meta_function::subtraction, [](const Vector2L& a, const Vector2L& b) { return Vector2L{ a.v - b.v }; },
        sol::meta_function::multiplication, sol::overload(
            [](const Vector2L& a, const Vector2L& b) { return Vector2L{ a.v * b.v }; },
            [](const Vector2L& a, float s) { return Vector2L{ a.v * s }; },
            [](float s, const Vector2L& a) { return Vector2L{ a.v * s }; }),
        sol::meta_function::division, sol::overload(
            [](const Vector2L& a, const Vector2L& b) { return Vector2L{ a.v / b.v }; },
            [](const Vector2L& a, float s) { return Vector2L{ a.v / s }; }),
        sol::meta_function::unary_minus, [](const Vector2L& a) { return Vector2L{ -a.v }; },
        sol::meta_function::equal_to, [](const Vector2L& a, const Vector2L& b) { return a.v == b.v; },
        sol::meta_function::to_string, [](const Vector2L& a) { return num(a.v.x) + ", " + num(a.v.y); });

    // ------------------------------------------------------------ Color3
    auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    lua.new_usertype<Color3>("Color3",
        "new", sol::factories([clamp01](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b) {
            return Color3(clamp01(r.value_or(0.0f)), clamp01(g.value_or(0.0f)), clamp01(b.value_or(0.0f)));
        }),
        "fromRGB", sol::factories([clamp01](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b) {
            return Color3(clamp01(r.value_or(0.0f) / 255.0f), clamp01(g.value_or(0.0f) / 255.0f), clamp01(b.value_or(0.0f) / 255.0f));
        }),
        "fromHSV", sol::factories([](float h, float s, float v) {
            h = h - std::floor(h);
            const float c = v * s, hp = h * 6.0f;
            const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
            float r = 0, g = 0, b = 0;
            if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
            else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
            const float m = v - c;
            return Color3(r + m, g + m, b + m);
        }),
        "fromHex", sol::factories([](std::string hex) {
            if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
            if (hex.size() != 6) fail("Color3.fromHex expects \"#RRGGBB\"");
            const unsigned long v = std::strtoul(hex.c_str(), nullptr, 16);
            return Color3::fromRGB(static_cast<int>((v >> 16) & 255), static_cast<int>((v >> 8) & 255), static_cast<int>(v & 255));
        }),
        "R", sol::property([](const Color3& c) { return c.r; }),
        "G", sol::property([](const Color3& c) { return c.g; }),
        "B", sol::property([](const Color3& c) { return c.b; }),
        "Lerp", [](const Color3& a, const Color3& b, float t) { return Color3(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t); },
        "ToHSV", [](const Color3& c) {
            const float mx = std::max({ c.r, c.g, c.b }), mn = std::min({ c.r, c.g, c.b }), d = mx - mn;
            float h = 0.0f;
            if (d > 1e-6f) {
                if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.0f);
                else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
                else h = (c.r - c.g) / d + 4.0f;
                h /= 6.0f;
                if (h < 0.0f) h += 1.0f;
            }
            return std::make_tuple(h, mx > 0.0f ? d / mx : 0.0f, mx);
        },
        "ToHex", [](const Color3& c) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02X%02X%02X", static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)), static_cast<int>(std::lround(c.b * 255)));
            return std::string(buf);
        },
        sol::meta_function::equal_to, [](const Color3& a, const Color3& b) { return a == b; },
        sol::meta_function::to_string, [](const Color3& c) { return num(c.r) + ", " + num(c.g) + ", " + num(c.b); });

    // ------------------------------------------------------------ UDim / UDim2
    lua.new_usertype<UDim>("UDim",
        "new", sol::factories([](sol::optional<float> s, sol::optional<float> o) { return UDim{ s.value_or(0.0f), o.value_or(0.0f) }; }),
        "Scale", sol::property([](const UDim& u) { return u.scale; }),
        "Offset", sol::property([](const UDim& u) { return u.offset; }),
        sol::meta_function::addition, [](const UDim& a, const UDim& b) { return UDim{ a.scale + b.scale, a.offset + b.offset }; },
        sol::meta_function::subtraction, [](const UDim& a, const UDim& b) { return UDim{ a.scale - b.scale, a.offset - b.offset }; },
        sol::meta_function::equal_to, [](const UDim& a, const UDim& b) { return a.scale == b.scale && a.offset == b.offset; },
        sol::meta_function::to_string, [](const UDim& u) { return num(u.scale) + ", " + num(u.offset); });

    lua.new_usertype<UDim2>("UDim2",
        "new", sol::factories(
            [](sol::optional<float> xs, sol::optional<float> xo, sol::optional<float> ys, sol::optional<float> yo) {
                return UDim2{ xs.value_or(0.0f), xo.value_or(0.0f), ys.value_or(0.0f), yo.value_or(0.0f) };
            },
            [](const UDim& x, const UDim& y) { return UDim2{ x.scale, x.offset, y.scale, y.offset }; }),
        "fromScale", sol::factories([](float x, float y) { return UDim2{ x, 0.0f, y, 0.0f }; }),
        "fromOffset", sol::factories([](float x, float y) { return UDim2{ 0.0f, x, 0.0f, y }; }),
        "X", sol::property([](const UDim2& u) { return UDim{ u.xScale, u.xOffset }; }),
        "Y", sol::property([](const UDim2& u) { return UDim{ u.yScale, u.yOffset }; }),
        "Width", sol::property([](const UDim2& u) { return UDim{ u.xScale, u.xOffset }; }),
        "Height", sol::property([](const UDim2& u) { return UDim{ u.yScale, u.yOffset }; }),
        "Lerp", [](const UDim2& a, const UDim2& b, float t) {
            return UDim2{ a.xScale + (b.xScale - a.xScale) * t, a.xOffset + (b.xOffset - a.xOffset) * t,
                          a.yScale + (b.yScale - a.yScale) * t, a.yOffset + (b.yOffset - a.yOffset) * t };
        },
        sol::meta_function::addition, [](const UDim2& a, const UDim2& b) { return UDim2{ a.xScale + b.xScale, a.xOffset + b.xOffset, a.yScale + b.yScale, a.yOffset + b.yOffset }; },
        sol::meta_function::subtraction, [](const UDim2& a, const UDim2& b) { return UDim2{ a.xScale - b.xScale, a.xOffset - b.xOffset, a.yScale - b.yScale, a.yOffset - b.yOffset }; },
        sol::meta_function::equal_to, [](const UDim2& a, const UDim2& b) { return a == b; },
        sol::meta_function::to_string, [](const UDim2& u) { return "{" + num(u.xScale) + ", " + num(u.xOffset) + "}, {" + num(u.yScale) + ", " + num(u.yOffset) + "}"; });

    // ------------------------------------------------------------ NumberRange, NumberSequence, ColorSequence
    lua.new_usertype<NumberRange>("NumberRange",
        "new", sol::factories([](float lo, sol::optional<float> hi) {
            const float h = hi.value_or(lo);
            if (h < lo) fail("NumberRange.new: max must not be less than min");
            return NumberRange(lo, h);
        }),
        "Min", sol::property([](const NumberRange& r) { return r.min; }),
        "Max", sol::property([](const NumberRange& r) { return r.max; }),
        sol::meta_function::equal_to, [](const NumberRange& a, const NumberRange& b) { return a == b; },
        sol::meta_function::to_string, [](const NumberRange& r) { return num(r.min) + " " + num(r.max); });

    lua.new_usertype<NumberKeypoint>("NumberSequenceKeypoint",
        "new", sol::factories([](float t, float v, sol::optional<float> e) { return NumberKeypoint{ t, v, e.value_or(0.0f) }; }),
        "Time", sol::property([](const NumberKeypoint& k) { return k.time; }),
        "Value", sol::property([](const NumberKeypoint& k) { return k.value; }),
        "Envelope", sol::property([](const NumberKeypoint& k) { return k.envelope; }),
        sol::meta_function::equal_to, [](const NumberKeypoint& a, const NumberKeypoint& b) { return a == b; },
        sol::meta_function::to_string, [](const NumberKeypoint& k) { return num(k.time) + " " + num(k.value) + " " + num(k.envelope); });

    auto keypointList = [](const sol::table& t, const char* what) {
        if (t.size() < 2) fail(std::string(what) + ".new needs at least 2 keypoints");
        if (t.size() > 20) fail(std::string(what) + ".new takes at most 20 keypoints");
    };
    lua.new_usertype<NumberSequence>("NumberSequence",
        "new", sol::factories(
            [](float v) { return NumberSequence(v); },
            [](float a, float b) { return NumberSequence(a, b); },
            [keypointList](const sol::table& t) {
                keypointList(t, "NumberSequence");
                std::vector<NumberKeypoint> keys;
                for (size_t i = 1; i <= t.size(); ++i) {
                    sol::object o = t[i];
                    if (!o.is<NumberKeypoint>()) fail("NumberSequence.new expects NumberSequenceKeypoints");
                    keys.push_back(o.as<NumberKeypoint>());
                }
                return NumberSequence(std::move(keys));
            }),
        "Keypoints", sol::property([this](const NumberSequence& s) {
            sol::table out = lua.create_table();
            for (size_t i = 0; i < s.keys.size(); ++i) out[i + 1] = s.keys[i];
            return out;
        }),
        sol::meta_function::equal_to, [](const NumberSequence& a, const NumberSequence& b) { return a == b; },
        sol::meta_function::to_string, [](const NumberSequence& s) {
            std::string out;
            for (const NumberKeypoint& k : s.keys) out += num(k.time) + " " + num(k.value) + " " + num(k.envelope) + " ";
            return out;
        });

    lua.new_usertype<ColorKeypoint>("ColorSequenceKeypoint",
        "new", sol::factories([](float t, const Color3& c) { return ColorKeypoint{ t, c }; }),
        "Time", sol::property([](const ColorKeypoint& k) { return k.time; }),
        "Value", sol::property([](const ColorKeypoint& k) { return k.color; }),
        sol::meta_function::equal_to, [](const ColorKeypoint& a, const ColorKeypoint& b) { return a == b; },
        sol::meta_function::to_string, [](const ColorKeypoint& k) { return num(k.time) + " " + num(k.color.r) + " " + num(k.color.g) + " " + num(k.color.b); });

    lua.new_usertype<ColorSequence>("ColorSequence",
        "new", sol::factories(
            [](const Color3& c) { return ColorSequence(c); },
            [](const Color3& a, const Color3& b) { return ColorSequence(a, b); },
            [keypointList](const sol::table& t) {
                keypointList(t, "ColorSequence");
                std::vector<ColorKeypoint> keys;
                for (size_t i = 1; i <= t.size(); ++i) {
                    sol::object o = t[i];
                    if (!o.is<ColorKeypoint>()) fail("ColorSequence.new expects ColorSequenceKeypoints");
                    keys.push_back(o.as<ColorKeypoint>());
                }
                return ColorSequence(std::move(keys));
            }),
        "Keypoints", sol::property([this](const ColorSequence& s) {
            sol::table out = lua.create_table();
            for (size_t i = 0; i < s.keys.size(); ++i) out[i + 1] = s.keys[i];
            return out;
        }),
        sol::meta_function::equal_to, [](const ColorSequence& a, const ColorSequence& b) { return a == b; },
        sol::meta_function::to_string, [](const ColorSequence& s) {
            std::string out;
            for (const ColorKeypoint& k : s.keys) out += num(k.time) + " " + num(k.color.r) + " " + num(k.color.g) + " " + num(k.color.b) + " ";
            return out;
        });

    // ------------------------------------------------------------ CFrame
    lua.new_usertype<CFrame>("CFrame",
        "new", sol::factories(
            []() { return CFrame{}; },
            [](const Vector3L& pos) { CFrame c; c.p = pos.v; return c; },
            [](const Vector3L& pos, const Vector3L& target) { return lookAtFrame(pos.v, target.v, glm::vec3(0, 1, 0)); },
            [](float x, float y, float z) { CFrame c; c.p = { x, y, z }; return c; },
            [](float x, float y, float z, float r00, float r01, float r02, float r10, float r11, float r12, float r20, float r21, float r22) {
                CFrame c;
                c.p = { x, y, z };
                // Arguments are rows; glm is column-major.
                c.r = glm::mat3(r00, r10, r20, r01, r11, r21, r02, r12, r22);
                return c;
            }),
        "lookAt", sol::factories([](const Vector3L& pos, const Vector3L& target, sol::optional<Vector3L> up) {
            return lookAtFrame(pos.v, target.v, up ? up->v : glm::vec3(0, 1, 0));
        }),
        "Angles", sol::factories([](float x, float y, float z) { CFrame c; c.r = anglesXYZ(x, y, z); return c; }),
        "fromEulerAnglesXYZ", sol::factories([](float x, float y, float z) { CFrame c; c.r = anglesXYZ(x, y, z); return c; }),
        "fromEulerAnglesYXZ", sol::factories([](float x, float y, float z) { CFrame c; c.r = glm::mat3(glm::eulerAngleYXZ(y, x, z)); return c; }),
        "fromOrientation", sol::factories([](float x, float y, float z) { CFrame c; c.r = glm::mat3(glm::eulerAngleYXZ(y, x, z)); return c; }),
        "fromAxisAngle", sol::factories([](const Vector3L& axis, float angle) {
            CFrame c;
            if (glm::length(axis.v) > 1e-8f) c.r = glm::mat3(glm::rotate(glm::mat4(1.0f), angle, glm::normalize(axis.v)));
            return c;
        }),
        "identity", sol::var(CFrame{}),
        "Position", sol::property([](const CFrame& c) { return Vector3L{ c.p }; }),
        "p", sol::property([](const CFrame& c) { return Vector3L{ c.p }; }),
        "X", sol::property([](const CFrame& c) { return c.p.x; }),
        "Y", sol::property([](const CFrame& c) { return c.p.y; }),
        "Z", sol::property([](const CFrame& c) { return c.p.z; }),
        "LookVector", sol::property([](const CFrame& c) { return Vector3L{ -c.r[2] }; }),
        "RightVector", sol::property([](const CFrame& c) { return Vector3L{ c.r[0] }; }),
        "UpVector", sol::property([](const CFrame& c) { return Vector3L{ c.r[1] }; }),
        "XVector", sol::property([](const CFrame& c) { return Vector3L{ c.r[0] }; }),
        "YVector", sol::property([](const CFrame& c) { return Vector3L{ c.r[1] }; }),
        "ZVector", sol::property([](const CFrame& c) { return Vector3L{ c.r[2] }; }),
        "Rotation", sol::property([](const CFrame& c) { CFrame r; r.r = c.r; return r; }),
        "Inverse", [](const CFrame& c) { return inverse(c); },
        "Lerp", [](const CFrame& a, const CFrame& b, float t) { return lerp(a, b, t); },
        "ToWorldSpace", [](const CFrame& a, const CFrame& b) { return mul(a, b); },
        "ToObjectSpace", [](const CFrame& a, const CFrame& b) { return mul(inverse(a), b); },
        "PointToWorldSpace", [](const CFrame& c, const Vector3L& v) { return Vector3L{ c.p + c.r * v.v }; },
        "PointToObjectSpace", [](const CFrame& c, const Vector3L& v) { return Vector3L{ glm::transpose(c.r) * (v.v - c.p) }; },
        "VectorToWorldSpace", [](const CFrame& c, const Vector3L& v) { return Vector3L{ c.r * v.v }; },
        "VectorToObjectSpace", [](const CFrame& c, const Vector3L& v) { return Vector3L{ glm::transpose(c.r) * v.v }; },
        "ToEulerAnglesXYZ", [](const CFrame& c) { const glm::vec3 e = toEulerXYZ(c.r); return std::make_tuple(e.x, e.y, e.z); },
        "ToOrientation", [](const CFrame& c) {
            float y, x, z;
            glm::extractEulerAngleYXZ(glm::mat4(c.r), y, x, z);
            return std::make_tuple(x, y, z);
        },
        "GetComponents", [](const CFrame& c) {
            return std::make_tuple(c.p.x, c.p.y, c.p.z, c.r[0][0], c.r[1][0], c.r[2][0], c.r[0][1], c.r[1][1], c.r[2][1], c.r[0][2], c.r[1][2], c.r[2][2]);
        },
        sol::meta_function::multiplication, sol::overload(
            [](const CFrame& a, const CFrame& b) { return mul(a, b); },
            [](const CFrame& a, const Vector3L& v) { return Vector3L{ a.p + a.r * v.v }; }),
        sol::meta_function::addition, [](const CFrame& a, const Vector3L& v) { CFrame c = a; c.p += v.v; return c; },
        sol::meta_function::subtraction, [](const CFrame& a, const Vector3L& v) { CFrame c = a; c.p -= v.v; return c; },
        sol::meta_function::equal_to, [](const CFrame& a, const CFrame& b) { return a.p == b.p && a.r == b.r; },
        sol::meta_function::to_string, [](const CFrame& c) {
            std::string s = num(c.p.x) + ", " + num(c.p.y) + ", " + num(c.p.z);
            for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) s += ", " + num(c.r[col][row]);
            return s;
        });

    // ------------------------------------------------------------ Enum
    lua.new_usertype<EnumItem>("EnumItem", sol::no_constructor,
        "Name", sol::property([](const EnumItem& e) { return e.name(); }),
        "Value", sol::property([](const EnumItem& e) { return e.value; }),
        "EnumType", sol::property([](const EnumItem& e) { return e.type ? e.type->name : std::string(); }),
        "IsA", [](const EnumItem& e, const std::string& typeName) { return e.type && e.type->name == typeName; },
        sol::meta_function::equal_to, [](const EnumItem& a, const EnumItem& b) { return a.type == b.type && a.value == b.value; },
        sol::meta_function::to_string, [](const EnumItem& e) { return "Enum." + (e.type ? e.type->name : std::string("?")) + "." + e.name(); });

    sol::table enumTable = lua.create_named_table("Enum");
    for (const EnumType* t : allEnumTypes()) {
        sol::table items = lua.create_table();
        sol::table list = lua.create_table();
        int n = 0;
        for (const auto& [name, value] : t->items) {
            if (items[name].valid() && items[name].get_type() != sol::type::lua_nil) continue; // aliases
            items[name] = EnumItem{ t, value };
            list[++n] = EnumItem{ t, value };
        }
        items["GetEnumItems"] = [list](sol::this_state) { return list; };
        enumTable[t->name] = items;
    }
}

// ============================================================================
//  Value <-> Lua
// ============================================================================
sol::object LuaRuntime::Impl::toLua(const Value& v, const PropertyInfo* prop) {
    switch (typeOf(v)) {
        case ValueType::None: return nil();
        case ValueType::Bool: return sol::make_object(lua, std::get<bool>(v));
        case ValueType::Int: return sol::make_object(lua, std::get<int>(v));
        case ValueType::Float: return sol::make_object(lua, static_cast<double>(std::get<float>(v)));
        case ValueType::String: return sol::make_object(lua, std::get<std::string>(v));
        case ValueType::Asset: return sol::make_object(lua, std::get<AssetPath>(v).path);
        case ValueType::Vector3: return sol::make_object(lua, Vector3L{ std::get<glm::vec3>(v) });
        case ValueType::Vector2: return sol::make_object(lua, Vector2L{ std::get<glm::vec2>(v) });
        case ValueType::Color3: return sol::make_object(lua, std::get<Color3>(v));
        case ValueType::UDim2: return sol::make_object(lua, std::get<UDim2>(v));
        case ValueType::NumberRange: return sol::make_object(lua, std::get<NumberRange>(v));
        case ValueType::NumberSequence: return sol::make_object(lua, std::get<NumberSequence>(v));
        case ValueType::ColorSequence: return sol::make_object(lua, std::get<ColorSequence>(v));
        case ValueType::Ref: {
            const uint64_t id = std::get<InstanceRef>(v).id;
            return id && scene.find(id) ? instanceFor(id) : nil();
        }
        case ValueType::Enum: {
            const int index = std::get<EnumValue>(v).index;
            if (prop && !prop->enumType.empty()) {
                if (const EnumType* t = enumType(prop->enumType)) return sol::make_object(lua, EnumItem{ t, index });
            }
            return sol::make_object(lua, index);
        }
    }
    return nil();
}

Value LuaRuntime::Impl::fromLua(const sol::object& o, const PropertyInfo& prop, std::string& error) {
    const sol::type t = o.get_type();
    auto wrong = [&](const char* expected) -> Value {
        error = prop.name + " expects " + expected;
        return {};
    };
    switch (prop.type) {
        case ValueType::Bool:
            if (t == sol::type::boolean) return o.as<bool>();
            return wrong("true or false");
        case ValueType::Int:
            if (t == sol::type::number) return static_cast<int>(std::llround(o.as<double>()));
            return wrong("a number");
        case ValueType::Float:
            if (t == sol::type::number) return static_cast<float>(o.as<double>());
            return wrong("a number");
        case ValueType::String:
            if (t == sol::type::string) return o.as<std::string>();
            if (t == sol::type::number) return num(o.as<double>()); // Roblox converts numbers for text
            return wrong("a string");
        case ValueType::Asset:
            if (t == sol::type::string) return AssetPath{ o.as<std::string>() };
            return wrong("a file path string");
        case ValueType::Vector3:
            if (o.is<Vector3L>()) return o.as<Vector3L>().v;
            return wrong("a Vector3");
        case ValueType::Vector2:
            if (o.is<Vector2L>()) return o.as<Vector2L>().v;
            return wrong("a Vector2");
        case ValueType::Color3:
            if (o.is<Color3>()) return o.as<Color3>();
            return wrong("a Color3");
        case ValueType::UDim2:
            if (o.is<UDim2>()) return o.as<UDim2>();
            return wrong("a UDim2");
        case ValueType::NumberRange:
            if (o.is<NumberRange>()) return o.as<NumberRange>();
            if (t == sol::type::number) return NumberRange(static_cast<float>(o.as<double>()));
            return wrong("a NumberRange");
        case ValueType::NumberSequence:
            if (o.is<NumberSequence>()) return o.as<NumberSequence>();
            if (t == sol::type::number) return NumberSequence(static_cast<float>(o.as<double>()));
            return wrong("a NumberSequence");
        case ValueType::ColorSequence:
            if (o.is<ColorSequence>()) return o.as<ColorSequence>();
            if (o.is<Color3>()) return ColorSequence(o.as<Color3>());
            return wrong("a ColorSequence");
        case ValueType::Ref: {
            if (t == sol::type::lua_nil || t == sol::type::none) return InstanceRef{};
            if (!o.is<Inst>()) return wrong("an Attachment");
            Instance* target = need(o.as<Inst>());
            if (target->cls != ClassId::Attachment) return wrong("an Attachment");
            return InstanceRef{ target->id };
        }
        case ValueType::Enum: {
            if (o.is<EnumItem>()) {
                const EnumItem e = o.as<EnumItem>();
                if (!e.type || e.type->name != prop.enumType) return wrong(("an Enum." + prop.enumType + " value").c_str());
                return EnumValue{ e.value };
            }
            if (t == sol::type::string) {
                const auto& names = prop.enumNames;
                auto it = std::find(names.begin(), names.end(), o.as<std::string>());
                if (it != names.end()) return EnumValue{ static_cast<int>(it - names.begin()) };
            }
            if (t == sol::type::number) return EnumValue{ static_cast<int>(o.as<double>()) };
            return wrong(("an Enum." + prop.enumType + " value").c_str());
        }
        case ValueType::None:
            break;
    }
    return wrong("a value");
}

Value LuaRuntime::Impl::attributeFromLua(const sol::object& o, std::string& error) {
    switch (o.get_type()) {
        case sol::type::lua_nil: case sol::type::none: return {};
        case sol::type::boolean: return o.as<bool>();
        case sol::type::number: return static_cast<float>(o.as<double>());
        case sol::type::string: return o.as<std::string>();
        default: break;
    }
    if (o.is<Vector3L>()) return o.as<Vector3L>().v;
    if (o.is<Vector2L>()) return o.as<Vector2L>().v;
    if (o.is<Color3>()) return o.as<Color3>();
    if (o.is<UDim2>()) return o.as<UDim2>();
    error = "attributes can hold nil, booleans, numbers, strings, Vector3, Vector2, Color3 or UDim2";
    return {};
}

sol::object LuaRuntime::Impl::attributeToLua(const Value& v) { return toLua(v, nullptr); }

} // namespace rift
