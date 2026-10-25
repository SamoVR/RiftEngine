#pragma once
//
// Templates.h -- ready-made game pieces for Insert > Gameplay (a part plus
// the Lua script that makes it work). The scripts use the same Roblox idioms
// tutorials teach, so they double as examples.
//
#include "scene/Scene.h"

#include <string>
#include <vector>

namespace rift {

struct PartTemplate {
    std::string id;          // "KillBrick"
    std::string title;       // "Kill Brick"
    std::string description;
    // What to create: a part with these settings ...
    ClassId cls = ClassId::Part;
    PartShape shape = PartShape::Block;
    glm::vec3 size{ 4.0f, 1.0f, 4.0f };
    Color3 color{ 0.64f, 0.64f, 0.64f };
    Material material = Material::Plastic;
    bool anchored = true;
    bool canCollide = true;
    float transparency = 0.0f;
    // ... and, if not empty, a Script inside it (or in ScriptService when
    // `sceneScript` is true) with this source.
    std::string scriptName;
    std::string scriptSource;
    bool sceneScript = false;
    // A Tool (in StarterPack): the part above is its Handle, `build` adds the rest.
    bool tool = false;
    glm::vec3 gripPos{ 0.0f };
    void (*build)(Scene& scene, Instance& handle) = nullptr;
};

const std::vector<PartTemplate>& partTemplates();
const PartTemplate* findTemplate(const std::string& id);

// Source of a new, empty Script / ModuleScript.
const char* newScriptSource();
const char* newModuleSource();
const char* newLocalScriptSource();

} // namespace rift
