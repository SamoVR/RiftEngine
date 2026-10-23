#pragma once
//
// Framebuffer.h -- an offscreen render target: a multisampled colour + depth
// buffer that is resolved into a plain texture (what the editor's Viewport
// panel shows with ImGui::Image, or the player blits to the window).
//
#include <GL/glew.h>
#include <glm/glm.hpp>

namespace rift {

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // (Re)creates the buffers if the size or sample count changed.
    void resize(int width, int height, int samples = 4);
    void bind() const;            // render into the multisampled buffer
    void resolve() const;         // multisampled -> texture
    GLuint texture() const { return resolveColor_; }
    glm::ivec2 size() const { return { width_, height_ }; }
    bool valid() const { return msaaFbo_ != 0; }
    // RGBA8 pixels of the resolved texture, top row first (screenshots, tests).
    void readPixels(unsigned char* rgba) const;

private:
    void release();
    int width_ = 0, height_ = 0, samples_ = 0;
    GLuint msaaFbo_ = 0, msaaColor_ = 0, msaaDepth_ = 0;
    GLuint resolveFbo_ = 0, resolveColor_ = 0;
};

// Depth-only render target for the sun's shadow map.
class ShadowMap {
public:
    ~ShadowMap();
    void resize(int size);
    void bind() const;
    GLuint texture() const { return depth_; }
    int size() const { return size_; }
private:
    int size_ = 0;
    GLuint fbo_ = 0, depth_ = 0;
};

} // namespace rift
