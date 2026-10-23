#include "scene/Value.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>

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
    }
    return "?";
}

ValueType valueTypeFromName(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(ValueType::Asset); ++i) {
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
    }
    return "?";
}

} // namespace rift
