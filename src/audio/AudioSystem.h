#pragma once
//
// AudioSystem.h -- plays sound files. The game session talks to this
// interface; MiniAudioSystem (AudioSystem.cpp) is the real implementation and
// tests run without audio (no AudioSystem at all).
//
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace rift {

class AudioSystem {
public:
    using Handle = uint32_t; // 0 = invalid

    struct PlayParams {
        float volume = 0.5f;
        float speed = 1.0f;
        bool looped = false;
        std::optional<glm::vec3> position; // set: 3D sound at that point
        float rollOffDistance = 60.0f;     // silent beyond this distance
        float startTime = 0.0f;            // seconds
    };

    virtual ~AudioSystem() = default;

    virtual Handle play(const std::filesystem::path& file, const PlayParams& params) = 0;
    virtual void stop(Handle h) = 0;
    virtual void setPaused(Handle h, bool paused) = 0;
    virtual void setVolume(Handle h, float volume) = 0;
    virtual void setSpeed(Handle h, float speed) = 0;
    virtual void setLooped(Handle h, bool looped) = 0;
    virtual void setPosition(Handle h, const glm::vec3& position) = 0;
    virtual bool isPlaying(Handle h) const = 0; // false once it ended or was stopped
    virtual float timePosition(Handle h) const = 0;
    virtual float length(Handle h) const = 0;

    virtual void setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) = 0;
    virtual void setMasterVolume(float volume) = 0;
    virtual void stopAll() = 0;

    // Creates the platform audio device; nullptr if none is available.
    static std::unique_ptr<AudioSystem> create();
};

} // namespace rift
