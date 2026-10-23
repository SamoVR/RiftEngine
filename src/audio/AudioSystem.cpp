// AudioSystem.cpp -- the miniaudio implementation of AudioSystem.
// Supports .wav, .mp3, .flac (miniaudio's own decoders) and .ogg (stb_vorbis).
#include "audio/AudioSystem.h"
#include "core/Log.h"
#include "core/Paths.h"

#pragma warning(push, 0) // third-party code
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#include "../../third_party/miniaudio/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#pragma warning(pop)

#include <algorithm>
#include <memory>
#include <unordered_map>

namespace rift {

namespace {

class MiniAudioSystem final : public AudioSystem {
public:
    MiniAudioSystem() {
        ok_ = ma_engine_init(nullptr, &engine_) == MA_SUCCESS;
        if (!ok_) LOG_WARN("Audio", "No audio device; the game runs without sound.");
    }
    ~MiniAudioSystem() override {
        stopAll();
        if (ok_) ma_engine_uninit(&engine_);
    }
    bool ok() const { return ok_; }

    Handle play(const std::filesystem::path& file, const PlayParams& p) override {
        if (!ok_) return 0;
        auto voice = std::make_unique<ma_sound>();
        // Long files (music) are streamed; short ones are decoded up front.
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        const ma_uint32 flags = (!ec && size > 2u * 1024u * 1024u) ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        if (ma_sound_init_from_file_w(&engine_, file.wstring().c_str(), flags, nullptr, nullptr, voice.get()) != MA_SUCCESS) {
            LOG_WARN("Audio", "Cannot play " + Paths::toUtf8(file) + " (missing, or not .wav/.mp3/.ogg/.flac)");
            return 0;
        }
        ma_sound* s = voice.get();
        ma_sound_set_volume(s, p.volume);
        ma_sound_set_pitch(s, p.speed);
        ma_sound_set_looping(s, p.looped ? MA_TRUE : MA_FALSE);
        if (p.position) {
            ma_sound_set_spatialization_enabled(s, MA_TRUE);
            ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
            ma_sound_set_min_distance(s, 4.0f);
            ma_sound_set_max_distance(s, std::max(p.rollOffDistance, 4.1f));
            ma_sound_set_position(s, p.position->x, p.position->y, p.position->z);
        } else {
            ma_sound_set_spatialization_enabled(s, MA_FALSE);
        }
        if (p.startTime > 0.0f) ma_sound_seek_to_pcm_frame(s, static_cast<ma_uint64>(p.startTime * ma_engine_get_sample_rate(&engine_)));
        ma_sound_start(s);
        const Handle h = next_++;
        voices_.emplace(h, std::move(voice));
        return h;
    }

    void stop(Handle h) override {
        auto it = voices_.find(h);
        if (it == voices_.end()) return;
        ma_sound_uninit(it->second.get());
        voices_.erase(it);
    }
    void setPaused(Handle h, bool paused) override {
        if (ma_sound* s = find(h)) paused ? ma_sound_stop(s) : ma_sound_start(s);
    }
    void setVolume(Handle h, float v) override { if (ma_sound* s = find(h)) ma_sound_set_volume(s, v); }
    void setSpeed(Handle h, float v) override { if (ma_sound* s = find(h)) ma_sound_set_pitch(s, std::max(v, 0.01f)); }
    void setLooped(Handle h, bool looped) override { if (ma_sound* s = find(h)) ma_sound_set_looping(s, looped ? MA_TRUE : MA_FALSE); }
    void setPosition(Handle h, const glm::vec3& p) override { if (ma_sound* s = find(h)) ma_sound_set_position(s, p.x, p.y, p.z); }
    bool isPlaying(Handle h) const override {
        auto it = voices_.find(h);
        return it != voices_.end() && ma_sound_is_playing(it->second.get()) && !ma_sound_at_end(it->second.get());
    }
    float timePosition(Handle h) const override {
        float t = 0.0f;
        auto it = voices_.find(h);
        if (it != voices_.end()) ma_sound_get_cursor_in_seconds(it->second.get(), &t);
        return t;
    }
    float length(Handle h) const override {
        float t = 0.0f;
        auto it = voices_.find(h);
        if (it != voices_.end()) ma_sound_get_length_in_seconds(it->second.get(), &t);
        return t;
    }
    void setListener(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up) override {
        if (!ok_) return;
        ma_engine_listener_set_position(&engine_, 0, pos.x, pos.y, pos.z);
        ma_engine_listener_set_direction(&engine_, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&engine_, 0, up.x, up.y, up.z);
    }
    void setMasterVolume(float v) override { if (ok_) ma_engine_set_volume(&engine_, v); }
    void stopAll() override {
        for (auto& [h, s] : voices_) ma_sound_uninit(s.get());
        voices_.clear();
    }

private:
    ma_sound* find(Handle h) {
        auto it = voices_.find(h);
        return it == voices_.end() ? nullptr : it->second.get();
    }
    ma_engine engine_{};
    bool ok_ = false;
    Handle next_ = 1;
    std::unordered_map<Handle, std::unique_ptr<ma_sound>> voices_;
};

} // namespace

std::unique_ptr<AudioSystem> AudioSystem::create() {
    auto audio = std::make_unique<MiniAudioSystem>();
    if (!audio->ok()) return nullptr;
    return audio;
}

} // namespace rift
