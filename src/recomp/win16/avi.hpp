// AVI reader with a Cinepak ("cvid") video decoder and PCM audio, enough for
// the game's movies (all Cinepak with 8-bit PCM sound).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace win16 {

class Avi {
public:
    bool open(const std::string& hostPath);

    int width = 0, height = 0;
    double fps = 10.0;
    int frameCount() const { return int(frames_.size()); }
    uint32_t lengthMs() const { return frames_.empty() ? 0 : uint32_t(frames_.size() * 1000.0 / fps); }

    // audio (PCM as stored; format tag 1)
    int audioRate = 0, audioChannels = 0, audioBits = 0;
    std::vector<uint8_t> audio;

    // Decode frames in order up to `index` (restarting from the first frame
    // when going back). The picture is 0x00RRGGBB, width x height.
    void decode_to(int index);
    int decoded() const { return decoded_; }
    const std::vector<uint32_t>& picture() const { return pic_; }

private:
    struct Chunk { size_t offset, size; };
    std::vector<uint8_t> file_;
    std::vector<Chunk> frames_;
    std::vector<uint32_t> pic_;
    int decoded_ = -1;

    struct Entry { uint8_t y[4]; int8_t u, v; };
    struct Strip { Entry v4[256]; Entry v1[256]; };
    std::vector<Strip> strips_;

    void decode_frame(const uint8_t* d, size_t n);
    void decode_codebook(Entry* cb, int id, const uint8_t* d, const uint8_t* end);
    void decode_vectors(Strip& s, int id, int x1, int y1, int x2, int y2, const uint8_t* d, const uint8_t* end);
    void put_block(int x, int y, const Entry* e[4], bool v1);
};

}  // namespace win16
