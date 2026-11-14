#include "app/audio.hpp"

#include <SDL3/SDL.h>

namespace myulk {

Audio::Audio() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio init failed: %s", SDL_GetError());
        return;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) SDL_Log("no audio device: %s", SDL_GetError());
}

Audio::~Audio() {
    stop(music_);
    stop(sound_);
    if (device_) SDL_CloseAudioDevice(device_);
}

bool Audio::start(Channel& ch, const std::string& path, bool loop) {
    stop(ch);
    if (!device_) return false;
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) {
        SDL_Log("cannot load %s: %s", path.c_str(), SDL_GetError());
        return false;
    }
    ch.data.assign(buf, buf + len);
    SDL_free(buf);
    // The device sets the stream's output format when it is bound.
    ch.stream = SDL_CreateAudioStream(&spec, &spec);
    if (!ch.stream || !SDL_BindAudioStream(device_, ch.stream)) {
        SDL_Log("audio stream: %s", SDL_GetError());
        stop(ch);
        return false;
    }
    SDL_PutAudioStreamData(ch.stream, ch.data.data(), int(ch.data.size()));
    if (!loop) SDL_FlushAudioStream(ch.stream);
    ch.loop = loop;
    ch.active = true;
    return true;
}

void Audio::stop(Channel& ch) {
    if (ch.stream) SDL_DestroyAudioStream(ch.stream);
    ch.stream = nullptr;
    ch.active = false;
    ch.done = nullptr;
    ch.data.clear();
}

void Audio::playMusic(const std::string& path) { start(music_, path, true); }

void Audio::playSound(const std::string& path, std::function<void()> done) {
    if (start(sound_, path, false)) {
        sound_.done = std::move(done);
    } else if (done) {
        done();   // missing file or no device: don't stall the scene
    }
}

void Audio::stopAll() {
    stop(music_);
    stop(sound_);
}

void Audio::update() {
    if (music_.active && music_.loop &&
        SDL_GetAudioStreamQueued(music_.stream) < int(music_.data.size())) {
        SDL_PutAudioStreamData(music_.stream, music_.data.data(), int(music_.data.size()));
    }
    if (sound_.active && SDL_GetAudioStreamQueued(sound_.stream) == 0) {
        auto done = std::move(sound_.done);
        stop(sound_);
        if (done) done();
    }
}

}  // namespace myulk
