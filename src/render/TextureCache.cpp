#include "render/TextureCache.h"

namespace rift {

std::shared_ptr<Texture> TextureCache::get(const std::string& storedPath) {
    if (storedPath.empty()) return nullptr;
    const std::string key = Texture::normalizePath(storedPath);
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    auto texture = std::make_shared<Texture>(key);
    if (!texture->isValid()) texture.reset();
    textures_.emplace(key, texture);
    return texture;
}

void TextureCache::invalidate(const std::string& storedPath) {
    textures_.erase(Texture::normalizePath(storedPath));
}

} // namespace rift
