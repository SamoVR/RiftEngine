#pragma once
//
// LuaApi.h -- what the Lua API offers, as data: which instances have which
// methods and events, the globals, libraries, constructors, the members of
// value types and the enums. The runtime uses the rules (isA, hasEvent,
// methodApplies); the script editor uses all of it for autocomplete. No sol2
// here, so the editor can include it cheaply.
//
#include "scene/Instance.h"

#include <string>
#include <utility>
#include <vector>

namespace rift::luaapi {

// ---- rules the runtime follows
// Roblox's abstract classes too (BasePart, GuiObject, ...), for IsA.
bool isA(const Instance& i, const std::string& className);
bool hasEvent(const Instance& i, const std::string& name);
// Methods that only some classes have (others fall through to child lookup).
bool methodApplies(const std::string& name, const Instance& i);

// ---- catalogue (sorted names)
const std::vector<std::string>& methodNames();          // every instance method
const std::vector<std::string>& eventNames();           // every instance event
// Members an instance of this class shows: its properties, computed ones
// (Parent, CFrame, ...), its methods and its events.
std::vector<std::string> instanceMembers(ClassId cls, bool methods);
const std::vector<std::string>& globals();              // game, workspace, print, Vector3, ...
const std::vector<std::string>& keywords();             // Lua keywords
// math, string, table, task, coroutine, os, utf8 ("" if not a library).
const std::vector<std::string>& libraryMembers(const std::string& library);
// Static members of a type table: Vector3.new / .zero, CFrame.Angles, Instance.new, ...
const std::vector<std::string>& typeStatics(const std::string& type);
// Members of a value: a Vector3's X / Magnitude / Dot, a CFrame's LookVector, a
// signal's Connect, an AnimationTrack's Play, ...
const std::vector<std::string>& valueMembers(const std::string& type);
// Services game:GetService knows.
const std::vector<std::string>& serviceNames();
// Enum types and their items (Enum.Material.Grass, Enum.KeyCode.E, ...).
// Defined with the Lua enums (LuaTypes.cpp).
const std::vector<std::pair<std::string, std::vector<std::string>>>& enums();

} // namespace rift::luaapi
