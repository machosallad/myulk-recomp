// Minimal audio for the port: one looping music channel (the original's
// sndPlaySound with SND_LOOP) and one effect/speech channel (the media
// players) with a completion callback. Uses SDL3 audio streams, which the
// device mixes for us.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace myulk {

class Audio {
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const { return device_ != 0; }
    void playMusic(const std::string& path);           // loops until stopped
    void playSound(const std::string& path, std::function<void()> done);
    void stopAll();
    void update();   // call every frame: keeps music looping, fires callbacks

private:
    struct Channel {
        SDL_AudioStream* stream = nullptr;
        std::vector<std::uint8_t> data;
        bool loop = false;
        bool active = false;
        std::function<void()> done;
    };
    bool start(Channel& ch, const std::string& path, bool loop);
    void stop(Channel& ch);

    std::uint32_t device_ = 0;
    Channel music_, sound_;
};

}  // namespace myulk
