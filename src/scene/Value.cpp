#include "scene/Value.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rift {

namespace {
    // 5 decimals is far below anything visible and keeps scene files short.
    double round5(float v) {
        const double r = std::round(static_cast<double>(v) * 1e5) / 1e5;
        return r == 0.0 ? 0.0 : r; // no "-0.0"
    }
    std::string num(float v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        return buf;
    }
    bool isNumberArray(const nlohmann::json& j, size_t n) {
        if (!j.is_array() || j.size() != n) return false;
        for (const auto& e : j) if (!e.is_number()) return false;
        return true;
    }
}

const char* typeName(ValueType t) {
    switch (t) {
        case ValueType::None: return "nil";
        case ValueType::Bool: return "bool";
        case ValueType::Int: return "int";
        case ValueType::Float: return "number";
        case ValueType::String: return "string";
        case ValueType::Vector3: return "Vector3";
        case ValueType::Vector2: return "Vector2";
        case ValueType::Color3: return "Color3";
        case ValueType::UDim2: return "UDim2";
        case ValueType::Enum: return "Enum";
        case ValueType::Asset: return "Asset";
        case ValueType::NumberRange: return "NumberRange";
        case ValueType::NumberSequence: return "NumberSequence";
        case ValueType::ColorSequence: return "ColorSequence";
        case ValueType::Ref: return "Instance";
    }
    return "?";
}

// ============================================================================
//  Sequences
// ============================================================================
namespace {
    // Sorted by time, clamped to 0..1, starting at 0 and ending at 1.
    template <class K> void normalise(std::vector<K>& keys, const K& fallback) {
        if (keys.empty()) keys.push_back(fallback);
        for (K& k : keys) k.time = std::clamp(k.time, 0.0f, 1.0f);
        std::stable_sort(keys.begin(), keys.end(), [](const K& a, const K& b) { return a.time < b.time; });
        if (keys.front().time > 0.0f) { K first = keys.front(); first.time = 0.0f; keys.insert(keys.begin(), first); }
        if (keys.back().time < 1.0f) { K last = keys.back(); last.time = 1.0f; keys.push_back(last); }
    }
    // The keypoint before t and how far t is towards the next one.
    template <class K> std::pair<size_t, float> span(const std::vector<K>& keys, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            if (t <= keys[i + 1].time) {
                const float w = keys[i + 1].time - keys[i].time;
                return { i, w > 1e-6f ? (t - keys[i].time) / w : 0.0f };
            }
        }
        return { keys.size() - 2, 1.0f };
    }
}

NumberSequence::NumberSequence(std::vector<NumberKeypoint> k) : keys(std::move(k)) { normalise(keys, NumberKeypoint{}); }

float NumberSequence::at(float t) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys[0].value;
    const auto [i, f] = span(keys, t);
    return keys[i].value + (keys[i + 1].value - keys[i].value) * f;
}

float NumberSequence::envelopeAt(float t) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys[0].envelope;
    const auto [i, f] = span(keys, t);
    return keys[i].envelope + (keys[i + 1].envelope - keys[i].envelope) * f;
}

ColorSequence::ColorSequence(std::vector<ColorKeypoint> k) : keys(std::move(k)) { normalise(keys, ColorKeypoint{}); }

Color3 ColorSequence::at(float t) const {
    if (keys.empty()) return { 1.0f, 1.0f, 1.0f };
    if (keys.size() == 1) return keys[0].color;
    const auto [i, f] = span(keys, t);
    const Color3& a = keys[i].color;
    const Color3& b = keys[i + 1].color;
    return { a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f };
}

ValueType valueTypeFromName(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(ValueType::Ref); ++i) {
        if (name == typeName(static_cast<ValueType>(i))) return static_cast<ValueType>(i);
    }
    return ValueType::None;
}

bool valuesEqual(const Value& a, const Value& b) {
    return a == b; // every alternative has operator==
}

nlohmann::json toJson(const Value& v) {
    using J = nlohmann::json;
    switch (typeOf(v)) {
        case ValueType::None: return nullptr;
        case ValueType::Bool: return std::get<bool>(v);
        case ValueType::Int: return std::get<int>(v);
        case ValueType::Float: return round5(std::get<float>(v));
        case ValueType::String: return std::get<std::string>(v);
        case ValueType::Vector3: { const auto& x = std::get<glm::vec3>(v); return J::array({ round5(x.x), round5(x.y), round5(x.z) }); }
        case ValueType::Vector2: { const auto& x = std::get<glm::vec2>(v); return J::array({ round5(x.x), round5(x.y) }); }
        case ValueType::Color3: { const auto& c = std::get<Color3>(v); return J::array({ round5(c.r), round5(c.g), round5(c.b) }); }
        case ValueType::UDim2: { const auto& u = std::get<UDim2>(v); return J::array({ round5(u.xScale), round5(u.xOffset), round5(u.yScale), round5(u.yOffset) }); }
        case ValueType::Enum: return std::get<EnumValue>(v).index;
        case ValueType::Asset: return std::get<AssetPath>(v).path;
        case ValueType::NumberRange: { const auto& r = std::get<NumberRange>(v); return J::array({ round5(r.min), round5(r.max) }); }
        case ValueType::NumberSequence: {
            J out = J::array();
            for (const NumberKeypoint& k : std::get<NumberSequence>(v).keys) out.push_back(J::array({ round5(k.time), round5(k.value), round5(k.envelope) }));
            return out;
        }
        case ValueType::ColorSequence: {
            J out = J::array();
            for (const ColorKeypoint& k : std::get<ColorSequence>(v).keys) out.push_back(J::array({ round5(k.time), round5(k.color.r), round5(k.color.g), round5(k.color.b) }));
            return out;
        }
        case ValueType::Ref: {
            const uint64_t id = std::get<InstanceRef>(v).id;
            if (!id) return nullptr;
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
            return std::string(buf);
        }
    }
    return nullptr;
}

Value fromJson(const nlohmann::json& j, ValueType type) {
    switch (type) {
        case ValueType::None: return {};
        case ValueType::Bool: if (j.is_boolean()) return j.get<bool>(); break;
        case ValueType::Int: if (j.is_number()) return static_cast<int>(j.get<double>()); break;
        case ValueType::Float: if (j.is_number()) return j.get<float>(); break;
        case ValueType::String: if (j.is_string()) return j.get<std::string>(); break;
        case ValueType::Vector3: if (isNumberArray(j, 3)) return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()); break;
        case ValueType::Vector2: if (isNumberArray(j, 2)) return glm::vec2(j[0].get<float>(), j[1].get<float>()); break;
        case ValueType::Color3: if (isNumberArray(j, 3)) return Color3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()); break;
        case ValueType::UDim2: if (isNumberArray(j, 4)) return UDim2{ j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() }; break;
        case ValueType::Enum: if (j.is_number_integer()) return EnumValue{ j.get<int>() }; break;
        case ValueType::Asset: if (j.is_string()) return AssetPath{ j.get<std::string>() }; break;
        case ValueType::NumberRange: if (isNumberArray(j, 2)) return NumberRange(j[0].get<float>(), j[1].get<float>()); break;
        case ValueType::NumberSequence: {
            if (!j.is_array() || j.empty()) break;
            std::vector<NumberKeypoint> keys;
            for (const auto& k : j) {
                if (isNumberArray(k, 3)) keys.push_back({ k[0].get<float>(), k[1].get<float>(), k[2].get<float>() });
                else if (isNumberArray(k, 2)) keys.push_back({ k[0].get<float>(), k[1].get<float>(), 0.0f });
                else return {};
            }
            return NumberSequence(std::move(keys));
        }
        case ValueType::ColorSequence: {
            if (!j.is_array() || j.empty()) break;
            std::vector<ColorKeypoint> keys;
            for (const auto& k : j) {
                if (!isNumberArray(k, 4)) return {};
                keys.push_back({ k[0].get<float>(), Color3(k[1].get<float>(), k[2].get<float>(), k[3].get<float>()) });
            }
            return ColorSequence(std::move(keys));
        }
        case ValueType::Ref:
            if (j.is_null()) return InstanceRef{};
            if (j.is_string()) {
                const std::string s = j.get<std::string>();
                if (s.empty()) return InstanceRef{};
                char* end = nullptr;
                const unsigned long long id = std::strtoull(s.c_str(), &end, 16);
                if (end && *end == 0) return InstanceRef{ static_cast<uint64_t>(id) };
            }
            break;
    }
    return {};
}

nlohmann::json toTaggedJson(const Value& v) {
    return { { "t", typeName(typeOf(v)) }, { "v", toJson(v) } };
}

Value fromTaggedJson(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string() || !j.contains("v")) return {};
    return fromJson(j["v"], valueTypeFromName(j["t"].get<std::string>()));
}

std::string toDisplayString(const Value& v) {
    switch (typeOf(v)) {
        case ValueType::None: return "nil";
        case ValueType::Bool: return std::get<bool>(v) ? "true" : "false";
        case ValueType::Int: return std::to_string(std::get<int>(v));
        case ValueType::Float: return num(std::get<float>(v));
        case ValueType::String: return std::get<std::string>(v);
        case ValueType::Vector3: { const auto& x = std::get<glm::vec3>(v); return num(x.x) + ", " + num(x.y) + ", " + num(x.z); }
        case ValueType::Vector2: { const auto& x = std::get<glm::vec2>(v); return num(x.x) + ", " + num(x.y); }
        case ValueType::Color3: { const auto& c = std::get<Color3>(v); return num(c.r) + ", " + num(c.g) + ", " + num(c.b); }
        case ValueType::UDim2: { const auto& u = std::get<UDim2>(v); return "{" + num(u.xScale) + ", " + num(u.xOffset) + "}, {" + num(u.yScale) + ", " + num(u.yOffset) + "}"; }
        case ValueType::Enum: return std::to_string(std::get<EnumValue>(v).index);
        case ValueType::Asset: return std::get<AssetPath>(v).path;
        case ValueType::NumberRange: { const auto& r = std::get<NumberRange>(v); return r.min == r.max ? num(r.min) : num(r.min) + " .. " + num(r.max); }
        case ValueType::NumberSequence: {
            std::string out;
            for (const NumberKeypoint& k : std::get<NumberSequence>(v).keys) out += (out.empty() ? "" : "  ") + num(k.time) + ": " + num(k.value);
            return out;
        }
        case ValueType::ColorSequence: return std::to_string(std::get<ColorSequence>(v).keys.size()) + " colours";
        case ValueType::Ref: return std::get<InstanceRef>(v).id ? "Instance" : "nil";
    }
    return "?";
}

} // namespace rift
