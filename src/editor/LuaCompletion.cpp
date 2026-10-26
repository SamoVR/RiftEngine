#include "editor/LuaCompletion.h"
#include "script/LuaApi.h"
#include "scene/Reflection.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

namespace rift {

namespace {
    bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
    bool validIdentifier(const std::string& s) {
        if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
        return std::all_of(s.begin(), s.end(), identChar);
    }

    // One step of an expression: the root name, then ".Name" / ":Name", each
    // maybe called with a first string argument (":WaitForChild("Door")").
    struct Segment {
        char sep = 0;          // 0 for the root
        std::string name;
        bool call = false;
        std::string arg;       // the call's first argument when it is a string literal
    };

    // The first string literal in a call's arguments ("" if none).
    std::string stringArg(const std::string& args) {
        for (size_t k = 0; k < args.size(); ++k) {
            const char q = args[k];
            if (q != '"' && q != '\'') continue;
            const size_t end = args.find(q, k + 1);
            return end == std::string::npos ? std::string() : args.substr(k + 1, end - k - 1);
        }
        return {};
    }

    // Reads the expression that ends at `end` backwards: `game:GetService("Players").LocalPlayer`
    // -> [game] [:GetService("Players")] [.LocalPlayer]. Empty if it is not a plain chain.
    std::vector<Segment> parseChain(const std::string& s, size_t end) {
        std::vector<Segment> rev;
        size_t i = end;
        while (i > 0) {
            Segment seg;
            // A call: (...) right before.
            if (s[i - 1] == ')') {
                int depth = 0;
                size_t j = i;
                bool found = false;
                while (j > 0) {
                    --j;
                    if (s[j] == ')') ++depth;
                    else if (s[j] == '(' && --depth == 0) { found = true; break; }
                }
                if (!found) return {};
                seg.call = true;
                seg.arg = stringArg(s.substr(j + 1, i - j - 2));
                i = j;
            }
            // The name: an identifier, or ["Name"].
            if (i > 0 && s[i - 1] == ']') {
                const size_t open = s.rfind('[', i - 1);
                if (open == std::string::npos) return {};
                seg.name = stringArg(s.substr(open + 1, i - open - 2));
                i = open;
                seg.sep = '.';
                rev.push_back(seg);
                continue;
            }
            const size_t nameEnd = i;
            while (i > 0 && identChar(s[i - 1])) --i;
            if (i == nameEnd) return {};
            seg.name = s.substr(i, nameEnd - i);
            if (std::isdigit(static_cast<unsigned char>(seg.name[0]))) return {};
            if (i > 0 && (s[i - 1] == '.' || s[i - 1] == ':') && !(i > 1 && s[i - 2] == '.')) {
                seg.sep = s[i - 1];
                --i;
                rev.push_back(seg);
                continue;
            }
            rev.push_back(seg);
            break;
        }
        std::reverse(rev.begin(), rev.end());
        if (rev.empty() || rev.front().sep != 0) return {};
        return rev;
    }

    // What an expression is, as far as the editor can tell.
    struct Res {
        enum Kind { None, Inst, Class, AnyInstance, Game, Library, TypeTable, Value, EnumRoot } kind = None;
        const Instance* inst = nullptr;
        ClassId cls = ClassId::Folder;
        std::string name; // library / type table / value type
    };
    Res none() { return {}; }
    Res instance(const Instance* i) { Res r; if (i) { r.kind = Res::Inst; r.inst = i; r.cls = i->cls; } return r; }
    Res ofClass(ClassId c) { Res r; r.kind = Res::Class; r.cls = c; return r; }
    Res ofClassName(const std::string& name) {
        if (name == "BasePart" || name == "PVInstance") return ofClass(ClassId::Part);
        if (const ClassInfo* c = classByName(name)) return ofClass(c->id);
        Res r;
        r.kind = Res::AnyInstance;
        return r;
    }
    Res value(const std::string& type) { Res r; r.kind = Res::Value; r.name = type; return r; }
    Res anyInstance() { Res r; r.kind = Res::AnyInstance; return r; }

    const std::vector<std::string>& serviceValues() {
        // Services that are not objects in the Explorer: their members come from the value table.
        static const std::vector<std::string> v = { "RunService", "UserInputService", "TweenService", "Debris", "CollectionService",
                                                     "HttpService", "DataStoreService", "KeyframeSequenceProvider" };
        return v;
    }

    struct Ctx {
        const Scene& scene;
        const Instance* script;
        const std::string& document;
    };

    Res serviceByName(const Ctx& c, const std::string& name) {
        if (std::find(serviceValues().begin(), serviceValues().end(), name) != serviceValues().end()) return value(name);
        const std::string n = name == "ServerScriptService" ? "ScriptService" : name;
        if (const ClassInfo* info = classByName(n); info && info->service) return instance(c.scene.service(info->id));
        return none();
    }

    Res resolveChain(const Ctx& c, const std::vector<Segment>& chain, int depth);

    // A local's value: the last "local name = <expression>" in the file.
    Res resolveLocal(const Ctx& c, const std::string& name, int depth) {
        if (depth > 4) return none();
        const std::regex assign("(?:^|[^A-Za-z0-9_.:])" + name + "\\s*=\\s*([^\\n=][^\\n]*)");
        std::string rhs;
        for (auto it = std::sregex_iterator(c.document.begin(), c.document.end(), assign); it != std::sregex_iterator(); ++it) rhs = (*it)[1].str();
        if (!rhs.empty()) {
            if (const size_t comment = rhs.find("--"); comment != std::string::npos) rhs.resize(comment);
            while (!rhs.empty() && std::isspace(static_cast<unsigned char>(rhs.back()))) rhs.pop_back();
            // "a or b": the first part is usually the interesting one.
            if (const size_t orAt = rhs.find(" or "); orAt != std::string::npos) rhs.resize(orAt);
            auto chain = parseChain(rhs, rhs.size());
            size_t lead = 0;
            while (lead < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[lead]))) ++lead;
            if (!chain.empty() && rhs.compare(lead, chain.front().name.size(), chain.front().name) == 0) {
                Res r = resolveChain(c, chain, depth + 1);
                if (r.kind != Res::None) return r;
            }
        }
        // Names people give the usual things (handler parameters).
        static const std::vector<std::pair<const char*, ClassId>> usual = {
            { "player", ClassId::Player }, { "plr", ClassId::Player }, { "character", ClassId::Model }, { "char", ClassId::Model },
            { "humanoid", ClassId::Humanoid }, { "hum", ClassId::Humanoid }, { "hit", ClassId::Part }, { "part", ClassId::Part },
            { "tool", ClassId::Tool }, { "model", ClassId::Model }, { "gui", ClassId::ScreenGui }, { "button", ClassId::TextButton },
            { "label", ClassId::TextLabel }, { "frame", ClassId::Frame }, { "sound", ClassId::Sound }, { "remote", ClassId::RemoteEvent },
        };
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        for (const auto& [n, cls] : usual) if (lower == n) return ofClass(cls);
        return none();
    }

    Res resolveRoot(const Ctx& c, const std::string& name, int depth) {
        if (name == "game" || name == "Game") { Res r; r.kind = Res::Game; return r; }
        if (name == "workspace" || name == "Workspace") return instance(&c.scene.workspace());
        if (name == "script") return c.script ? instance(c.script) : ofClass(ClassId::Script);
        if (name == "Enum") { Res r; r.kind = Res::EnumRoot; return r; }
        if (!luaapi::libraryMembers(name).empty()) { Res r; r.kind = Res::Library; r.name = name; return r; }
        if (!luaapi::typeStatics(name).empty()) { Res r; r.kind = Res::TypeTable; r.name = name; return r; }
        return resolveLocal(c, name, depth);
    }

    // The type a property gives (Vector3, CFrame, ...).
    Res propertyValue(ClassId cls, const std::string& name, const Instance* inst, const Scene& scene) {
        if (name == "Parent") return inst && inst->parent ? instance(inst->parent) : anyInstance();
        if (name == "CFrame" || name == "Pivot") return value("CFrame");
        if (name == "BrickColor") return value("BrickColor");
        if (name == "Character") return ofClass(ClassId::Model);
        if (name == "LocalPlayer") return ofClass(ClassId::Player);
        if (name == "PrimaryPart" || name == "RootPart") return ofClass(ClassId::Part);
        if (name == "Terrain" && cls == ClassId::Workspace) return scene.terrainInstance() ? instance(scene.terrainInstance()) : ofClass(ClassId::Terrain);
        if (name == "CurrentCamera") return ofClass(ClassId::Camera);
        const PropertyInfo* p = findProperty(cls, name);
        if (!p) {
            Instance probe;
            probe.cls = cls;
            if (luaapi::hasEvent(probe, name)) return value("RBXScriptSignal");
            return none();
        }
        switch (p->type) {
            case ValueType::Vector3: return value("Vector3");
            case ValueType::Vector2: return value("Vector2");
            case ValueType::Color3: return value("Color3");
            case ValueType::UDim2: return value("UDim2");
            case ValueType::CFrame: return value("CFrame");
            case ValueType::NumberRange: return value("NumberRange");
            case ValueType::NumberSequence: return value("NumberSequence");
            case ValueType::ColorSequence: return value("ColorSequence");
            case ValueType::Ref: return name.rfind("Part", 0) == 0 ? ofClass(ClassId::Part) : ofClass(ClassId::Attachment);
            default: return none();
        }
    }

    Res step(const Ctx& c, const Res& r, const Segment& s) {
        switch (r.kind) {
            case Res::Game:
                if (s.sep == '.') return serviceByName(c, s.name);
                if (s.sep == ':' && (s.name == "GetService" || s.name == "FindService")) return serviceByName(c, s.arg);
                if (s.sep == ':' && (s.name == "WaitForChild" || s.name == "FindFirstChild")) return serviceByName(c, s.arg);
                return none();
            case Res::Inst:
            case Res::Class:
            case Res::AnyInstance: {
                const Instance* inst = r.kind == Res::Inst ? r.inst : nullptr;
                const ClassId cls = r.kind == Res::AnyInstance ? ClassId::Folder : r.cls;
                if (s.sep == '.') {
                    if (inst) {
                        if (const Instance* child = inst->findFirstChild(s.name)) return instance(child);
                    }
                    Res p = propertyValue(cls, s.name, inst, c.scene);
                    if (p.kind != Res::None) return p;
                    // An unknown child (made by a script, or a character's part).
                    if (s.name == "Humanoid") return ofClass(ClassId::Humanoid);
                    if (s.name == "HumanoidRootPart" || s.name == "Head" || s.name == "Torso" || s.name == "Handle") return ofClass(ClassId::Part);
                    return r.kind == Res::Inst && !inst->findFirstChild(s.name) ? anyInstance() : anyInstance();
                }
                if (!s.call) return none();
                if (s.name == "WaitForChild" || s.name == "FindFirstChild" || s.name == "FindFirstDescendant") {
                    if (inst) {
                        if (const Instance* child = inst->findFirstChild(s.arg)) return instance(child);
                    }
                    if (s.arg == "Humanoid") return ofClass(ClassId::Humanoid);
                    if (s.arg == "Animator") return ofClass(ClassId::Animator);
                    if (s.arg == "HumanoidRootPart" || s.arg == "Head" || s.arg == "Handle") return ofClass(ClassId::Part);
                    return anyInstance();
                }
                if (s.name == "FindFirstChildOfClass" || s.name == "FindFirstChildWhichIsA" || s.name == "FindFirstAncestorOfClass"
                    || s.name == "FindFirstAncestorWhichIsA")
                    return ofClassName(s.arg);
                if (s.name == "FindFirstAncestor") return anyInstance();
                if (s.name == "Clone") return r;
                if (s.name == "GetService") return serviceByName(c, s.arg);
                if (s.name == "GetPropertyChangedSignal" || s.name == "GetAttributeChangedSignal") return value("RBXScriptSignal");
                if (s.name == "LoadAnimation") return value("AnimationTrack");
                if (s.name == "GetMouse") return value("Mouse");
                if (s.name == "Raycast") return value("RaycastResult");
                if (s.name == "GetPivot") return value("CFrame");
                if (s.name == "GetPlayerFromCharacter") return ofClass(ClassId::Player);
                return none();
            }
            case Res::TypeTable:
                if (r.name == "Instance" && s.name == "new") return ofClassName(s.arg);
                if (r.name == "Vector3" || r.name == "Vector2" || r.name == "CFrame" || r.name == "Color3" || r.name == "UDim2" || r.name == "BrickColor"
                    || r.name == "Random" || r.name == "NumberRange" || r.name == "NumberSequence" || r.name == "ColorSequence")
                    return value(r.name);
                return none();
            case Res::Value: {
                const std::string& t = r.name;
                if (t == "RBXScriptSignal" && s.sep == ':' && (s.name == "Connect" || s.name == "Once")) return value("RBXScriptConnection");
                if (t == "CFrame" && s.sep == '.') {
                    if (s.name == "Position" || s.name.find("Vector") != std::string::npos) return value("Vector3");
                    if (s.name == "Rotation") return value("CFrame");
                }
                if (t == "CFrame" && s.sep == ':' && (s.name == "Inverse" || s.name == "Lerp" || s.name == "ToWorldSpace" || s.name == "ToObjectSpace")) return value("CFrame");
                if (t == "CFrame" && s.sep == ':' && s.name.find("Space") != std::string::npos) return value("Vector3");
                if (t == "Vector3" && (s.name == "Unit" || s.name == "Cross" || s.name == "Lerp" || s.name == "Abs")) return value("Vector3");
                if (t == "AnimationTrack" && s.sep == '.' && (s.name == "Stopped" || s.name == "Ended" || s.name == "DidLoop" || s.name == "KeyframeReached"))
                    return value("RBXScriptSignal");
                if (t == "AnimationTrack" && s.name == "GetMarkerReachedSignal") return value("RBXScriptSignal");
                if (t == "Tween" && s.name == "Completed") return value("RBXScriptSignal");
                if (t == "TweenService" && s.name == "Create") return value("Tween");
                if (t == "Mouse" && s.name == "Hit") return value("CFrame");
                if (t == "Mouse" && s.name == "Target") return ofClass(ClassId::Part);
                if (t == "RaycastResult" && s.name == "Instance") return ofClass(ClassId::Part);
                if (t == "RaycastResult" && (s.name == "Position" || s.name == "Normal")) return value("Vector3");
                if ((t == "RunService" || t == "UserInputService" || t == "Mouse") && s.sep == '.') {
                    for (const std::string& m : luaapi::valueMembers(t))
                        if (m == s.name && !s.call && std::isupper(static_cast<unsigned char>(m[0]))) return value("RBXScriptSignal");
                }
                return none();
            }
            default:
                return none();
        }
    }

    Res resolveChain(const Ctx& c, const std::vector<Segment>& chain, int depth) {
        Res r = resolveRoot(c, chain.front().name, depth);
        if (chain.front().call) r = none();
        for (size_t k = 1; k < chain.size() && r.kind != Res::None; ++k) r = step(c, r, chain[k]);
        return r;
    }

    // Names already used in the file (locals, functions, fields).
    std::vector<std::string> documentWords(const std::string& doc, const std::string& except) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> out;
        for (size_t i = 0; i < doc.size();) {
            if (!identChar(doc[i]) || std::isdigit(static_cast<unsigned char>(doc[i]))) {
                // Skip string literals and comments roughly, so their words do not show up.
                if (doc[i] == '-' && i + 1 < doc.size() && doc[i + 1] == '-') { i = doc.find('\n', i); if (i == std::string::npos) break; continue; }
                if (doc[i] == '"' || doc[i] == '\'') { const size_t end = doc.find(doc[i], i + 1); i = end == std::string::npos ? doc.size() : end + 1; continue; }
                ++i;
                continue;
            }
            size_t j = i;
            while (j < doc.size() && identChar(doc[j])) ++j;
            std::string w = doc.substr(i, j - i);
            if (w.size() >= 2 && w != except && seen.insert(w).second) out.push_back(std::move(w));
            i = j;
        }
        return out;
    }

    bool startsWith(const std::string& s, const std::string& p, bool caseless) {
        if (p.size() > s.size()) return false;
        for (size_t k = 0; k < p.size(); ++k) {
            const char a = s[k], b = p[k];
            if (caseless ? std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)) : a != b) return false;
        }
        return true;
    }
    bool containsCaseless(const std::string& s, const std::string& p) {
        if (p.empty()) return true;
        auto it = std::search(s.begin(), s.end(), p.begin(), p.end(),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        return it != s.end();
    }
}

std::vector<LuaCompletion> luaCompletionItems(const std::string& line, size_t termStart, const std::string& term, const Scene& scene,
                                              const Instance* script, const std::string& document) {
    Ctx ctx{ scene, script, document };
    std::vector<LuaCompletion> candidates;
    auto add = [&](const std::string& text, std::string detail, std::string description = {}) {
        if (description.empty()) description = luaDoc(text);
        candidates.push_back({ text, std::move(detail), std::move(description) });
    };
    termStart = std::min(termStart, line.size());
    const char sep = termStart > 0 ? line[termStart - 1] : 0;
    const bool member = (sep == '.' || sep == ':') && !(termStart > 1 && line[termStart - 2] == '.');
    if (member) {
        const auto chain = parseChain(line, termStart - 1);
        if (chain.empty()) return {};
        const Res r = resolveChain(ctx, chain, 0);
        switch (r.kind) {
            case Res::Inst:
            case Res::Class:
            case Res::AnyInstance: {
                const ClassId cls = r.kind == Res::AnyInstance ? ClassId::Folder : r.cls;
                if (sep == ':') {
                    for (const std::string& m : luaapi::instanceMembers(cls, true)) add(m, "method");
                } else {
                    // Children first: they are what you usually want.
                    if (r.kind == Res::Inst)
                        for (const Instance* child : r.inst->children)
                            if (validIdentifier(child->name)) add(child->name, classInfo(child->cls).name, classInfo(child->cls).description);
                    Instance probe;
                    probe.cls = cls;
                    for (const std::string& m : luaapi::instanceMembers(cls, false)) {
                        if (luaapi::hasEvent(probe, m)) {
                            add(m, "event");
                            continue;
                        }
                        const PropertyInfo* p = findProperty(cls, m);
                        std::string type = p ? (p->enumType.empty() ? std::string(typeName(p->type)) : "Enum." + p->enumType) : std::string();
                        if (type == "Instance") type = "object";
                        const std::string& doc = luaDoc(m);
                        add(m, type.empty() ? "property" : "property: " + type, doc.empty() && p ? p->tooltip : doc);
                    }
                }
                break;
            }
            case Res::Game:
                if (sep == ':') {
                    for (const char* m : { "FindFirstChild", "FindService", "GetChildren", "GetService", "IsA", "WaitForChild" }) add(m, "method");
                } else {
                    for (const std::string& svc : luaapi::serviceNames()) {
                        const ClassInfo* info = classByName(svc);
                        add(svc, "service", info ? info->description : std::string());
                    }
                }
                break;
            case Res::Library:
                if (sep == '.') for (const std::string& m : luaapi::libraryMembers(r.name)) add(m, r.name);
                break;
            case Res::TypeTable:
                if (sep == '.') for (const std::string& m : luaapi::typeStatics(r.name)) add(m, r.name);
                break;
            case Res::Value:
                for (const std::string& m : luaapi::valueMembers(r.name)) add(m, r.name);
                break;
            case Res::EnumRoot:
                if (sep == '.') for (const auto& [name, items] : luaapi::enums()) add(name, "enum");
                break;
            case Res::None: {
                // Enum.Material.<here>
                if (chain.size() == 2 && chain[0].name == "Enum" && sep == '.') {
                    for (const auto& [name, items] : luaapi::enums())
                        if (name == chain[1].name) for (const std::string& item : items) add(item, "Enum." + name);
                }
                break;
            }
        }
    } else {
        // A word on its own: keywords, globals, the file's own names.
        for (const std::string& k : luaapi::keywords()) add(k, "keyword");
        for (const std::string& g : luaapi::globals()) add(g, "global");
        for (const std::string& w : documentWords(document, term)) add(w, "in this script");
    }

    // Exact-case prefix, then any-case prefix, then anywhere; no duplicates.
    std::vector<std::pair<int, size_t>> ranked;
    std::unordered_set<std::string> seen;
    for (size_t k = 0; k < candidates.size(); ++k) {
        const std::string& cand = candidates[k].text;
        if (!seen.insert(cand).second) continue;
        int rank = -1;
        if (startsWith(cand, term, false)) rank = 0;
        else if (startsWith(cand, term, true)) rank = 1;
        else if (term.size() >= 2 && containsCaseless(cand, term)) rank = 2;
        if (rank < 0 || (cand == term && !member)) continue;
        ranked.emplace_back(rank, k);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<LuaCompletion> out;
    for (const auto& [rank, k] : ranked) {
        out.push_back(std::move(candidates[k]));
        if (out.size() >= 80) break;
    }
    return out;
}

std::vector<std::string> luaCompletions(const std::string& line, size_t termStart, const std::string& term, const Scene& scene,
                                        const Instance* script, const std::string& document) {
    std::vector<std::string> out;
    for (LuaCompletion& c : luaCompletionItems(line, termStart, term, scene, script, document)) out.push_back(std::move(c.text));
    return out;
}

} // namespace rift
