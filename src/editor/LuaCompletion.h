#pragma once
//
// LuaCompletion.h -- what the script editor suggests while you type.
//
// After "." or ":" the expression before it is worked out, as far as it can
// be without running anything: globals (game, workspace, script, Enum, math,
// Vector3, ...), the objects of the scene the script lives in (so
// workspace.Map.Door. lists Door's children, properties and events),
// game:GetService("X"), :WaitForChild("X"), Instance.new("Part"), events and
// their :Connect, and simple locals (local door = workspace.Door). Anywhere
// else: Lua keywords, globals and the names already used in the file.
//
#include "scene/Scene.h"

#include <string>
#include <vector>

namespace rift {

// `line` is the text of the current line, `termStart` the byte where the word
// being typed starts (the word is `term`); `document` is the whole script.
// `script` is the Script instance the file belongs to (nullptr if none).
std::vector<std::string> luaCompletions(const std::string& line, size_t termStart, const std::string& term, const Scene& scene,
                                        const Instance* script, const std::string& document);

// The same with what each one is: `detail` is short ("property · Vector3",
// "method", "Part"), `description` a sentence (may be empty).
struct LuaCompletion {
    std::string text, detail, description;
};
std::vector<LuaCompletion> luaCompletionItems(const std::string& line, size_t termStart, const std::string& term, const Scene& scene,
                                              const Instance* script, const std::string& document);

// A one-line description of a global, method, event or common property ("" if unknown).
const std::string& luaDoc(const std::string& name);

} // namespace rift
