#pragma once
//
// TextureCache.h -- one GPU texture per image file, shared by everything that
// uses it (parts, GUI images, the editor's thumbnails). Keyed by the stored
// path ("assets/textures/wood.png", "engine:textures/checker.png").
// A file that fails to load is remembered, so it is not retried every frame;
// clear() (project switch, asset reimport) forgets everything.
//
#include "render/Texture.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace rift {

class TextureCache {
public:
    // nullptr if the file cannot be loaded.
    std::shared_ptr<Texture> get(const std::string& storedPath);
    void clear() { textures_.clear(); }
    // Forget one file (it changed on disk).
    void invalidate(const std::string& storedPath);
    size_t size() const { return textures_.size(); }

private:
    std::unordered_map<std::string, std::shared_ptr<Texture>> textures_; // null entry = failed
};

} // namespace rift
