#pragma once
//
// Reflection.h -- the property tables of every Instance class.
//
// A property is declared once (name, type, category, range, flags) and then
// used everywhere: the editor's Properties panel builds its widgets from it,
// scene files save it by name, undo records it as a Value, and Lua scripts
// read/write it as part.Name. Adding a property to a class = one line in
// Reflection.cpp.
//
#include "scene/Instance.h"
#include "scene/Value.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rift {

enum PropFlags : uint32_t {
    PF_Serialize    = 1u << 0, // saved in scene files
    PF_Editor       = 1u << 1, // shown in the Properties panel
    PF_ScriptRead   = 1u << 2,
    PF_ScriptWrite  = 1u << 3,
    PF_ReadOnlyUI   = 1u << 4, // shown in the editor but not editable there
    PF_Runtime      = 1u << 5, // simulation state (Velocity): scripts only, never saved
    PF_Default = PF_Serialize | PF_Editor | PF_ScriptRead | PF_ScriptWrite,
};

struct PropertyInfo {
    std::string name;
    ValueType type = ValueType::None;
    std::string category;
    Value (*get)(const Instance&) = nullptr;
    bool (*set)(Instance&, const Value&) = nullptr; // exact type expected (see setProperty)
    uint32_t flags = PF_Default;
    float minValue = 0.0f, maxValue = 0.0f;  // equal = unbounded
    float step = 0.0f;                       // drag speed hint for the editor (0 = auto)
    std::string enumType;              // Lua enum type ("Material"); empty if not an enum
    std::vector<std::string> enumNames;
    std::string tooltip;

    bool has(PropFlags f) const { return (flags & f) != 0; }
    bool bounded() const { return maxValue > minValue; }

    // Builder-style tweaks used by the tables in Reflection.cpp.
    PropertyInfo& range(float lo, float hi) { minValue = lo; maxValue = hi; return *this; }
    PropertyInfo& speed(float s) { step = s; return *this; }
    PropertyInfo& tip(const char* t) { tooltip = t; return *this; }
    PropertyInfo& flagsSet(uint32_t f) { flags = f; return *this; }
    PropertyInfo& enums(const char* typeName); // see allEnums()
};

struct ClassInfo {
    ClassId id = ClassId::Folder;
    std::string name;
    std::string description;
    bool creatable = true;   // Instance.new and the Insert menu
    bool service = false;
    std::vector<PropertyInfo> props; // display order
    std::unordered_map<std::string, int> byName; // name -> index into props

    const PropertyInfo* find(const std::string& propName) const;
    int indexOf(const std::string& propName) const; // -1 if absent
};

const ClassInfo& classInfo(ClassId cls);
const ClassInfo* classByName(const std::string& name);
const char* className(ClassId cls);
const PropertyInfo* findProperty(ClassId cls, const std::string& name);

// Class-specific starting values (a new TextButton has a blue background,
// a SpotLight a 45 degree cone, ...). Called by Scene::create.
void applyClassDefaults(Instance& instance);

// The value a property has on a freshly created instance of the class
// (scene files only store properties that differ from it).
const Value& defaultValue(ClassId cls, const PropertyInfo& prop);

// Reads a property.
Value getProperty(const Instance& instance, const PropertyInfo& prop);
// Writes a property, converting compatible values (number <-> int, enum by
// name or index, string -> asset) and clamping to the property's range.
// Returns false (and leaves the instance alone) if the value does not fit.
bool setProperty(Instance& instance, const PropertyInfo& prop, const Value& value, std::string* error = nullptr);
// Converts `value` to the property's type without writing it (monostate if impossible).
Value coerce(const PropertyInfo& prop, const Value& value);

// Names of an enum type by the type name used in Lua (Enum.PartType.Ball):
// "PartType", "Material", ... Empty if unknown.
const std::vector<std::string>& enumNamesFor(const std::string& enumType);
// All enum types exposed to scripts, as (type name, names).
const std::vector<std::pair<std::string, std::vector<std::string>>>& allEnums();

} // namespace rift
