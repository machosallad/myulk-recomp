// MMSYSTEM: sndPlaySound and the MCI subset used by Delphi's TMediaPlayer
// (waveaudio through SDL; AVI playback is not implemented yet and completes
// immediately).
#include <SDL3/SDL.h>

#include <map>
#include <vector>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;

namespace win16 {
namespace {

SDL_AudioDeviceID g_dev = 0;

struct Sound {
    SDL_AudioStream* stream = nullptr;
    std::vector<uint8_t> data;
    bool loop = false;
    bool playing = false;
    uint32_t lengthMs = 0;
};

void sound_stop(Sound& s) {
    if (s.stream) SDL_DestroyAudioStream(s.stream);
    s.stream = nullptr;
    s.playing = false;
}

bool sound_load(Sound& s, const std::string& dosPath) {
    sound_stop(s);
    std::string host = vfs::to_host(dosPath);
    if (host.empty()) {
        recomp::log("sound not found: %s", dosPath.c_str());
        return false;
    }
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(host.c_str(), &spec, &buf, &len)) return false;
    s.data.assign(buf, buf + len);
    SDL_free(buf);
    int bps = SDL_AUDIO_BYTESIZE(spec.format) * spec.channels;
    s.lengthMs = bps && spec.freq ? uint32_t(uint64_t(len) * 1000 / (uint64_t(bps) * spec.freq)) : 0;
    if (!g_dev) g_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    s.stream = SDL_CreateAudioStream(&spec, &spec);
    if (!s.stream || !g_dev || !SDL_BindAudioStream(g_dev, s.stream)) return false;
    return true;
}

void sound_play(Sound& s, bool loop) {
    if (!s.stream) return;
    SDL_ClearAudioStream(s.stream);
    SDL_PutAudioStreamData(s.stream, s.data.data(), int(s.data.size()));
    if (!loop) SDL_FlushAudioStream(s.stream);
    s.loop = loop;
    s.playing = true;
}

bool sound_busy(Sound& s) {
    if (!s.playing || !s.stream) return false;
    if (s.loop) {
        if (SDL_GetAudioStreamQueued(s.stream) < int(s.data.size()))
            SDL_PutAudioStreamData(s.stream, s.data.data(), int(s.data.size()));
        return true;
    }
    if (SDL_GetAudioStreamQueued(s.stream) > 0) return true;
    s.playing = false;
    return false;
}

Sound g_snd;   // sndPlaySound channel

struct McDevice {
    uint16_t id = 0;
    bool video = false;
    std::string element;
    Sound snd;
    uint16_t notifyHwnd = 0;
    bool notifyPending = false;
};
std::map<uint16_t, McDevice> g_mci;
uint16_t g_nextMci = 1;

}  // namespace

// Called while waiting for messages: finish sounds and post MCI notifications.
void mm_poll() {
    sound_busy(g_snd);
    for (auto& [id, d] : g_mci) {
        bool busy = d.video ? false : sound_busy(d.snd);
        if (!busy && d.notifyPending) {
            d.notifyPending = false;
            post_message(d.notifyHwnd, MM_MCINOTIFY, 1, id);   // MCI_NOTIFY_SUCCESSFUL
        }
    }
}

}  // namespace win16

using namespace win16;

API(MMSYSTEM, 2, sndPlaySound, 6) {
    uint16_t flags = a.w(); uint32_t name = a.p();
    if (!name) { sound_stop(g_snd); a.ret(1); return; }
    std::string file = recomp::read_cstr(c, name);
    if (sound_load(g_snd, file)) {
        sound_play(g_snd, flags & 8);   // SND_LOOP
        if (!(flags & 1)) {             // synchronous
            while (sound_busy(g_snd)) host_pump(10);
        }
    }
    a.ret(1);
}

API(MMSYSTEM, 701, mciSendCommand, 12) {
    uint32_t parms = a.p(); uint32_t flags = a.l(); uint16_t msg = a.w(); uint16_t dev = a.w();
    uint16_t s = uint16_t(parms >> 16), o = uint16_t(parms);
    auto it = g_mci.find(dev);
    McDevice* d = it == g_mci.end() ? nullptr : &it->second;
    auto set_notify = [&]() {
        if ((flags & 1) && d && parms) {   // MCI_NOTIFY
            d->notifyHwnd = c.rd16(s, o);
            d->notifyPending = true;
        }
    };
    uint32_t result = 0;
    switch (msg) {
        case 0x0803: {   // MCI_OPEN
            McDevice nd;
            nd.id = g_nextMci++;
            // MCI_OPEN_PARMS: dwCallback(0) wDeviceID(4) wReserved0(6) lpstrDeviceType(8)
            //                  lpstrElementName(12) lpstrAlias(16)
            std::string type = (flags & 0x2000) && !(flags & 0x1000) ? recomp::read_cstr(c, c.rd32(s, uint16_t(o + 8))) : "";
            if (flags & 0x0200) nd.element = recomp::read_cstr(c, c.rd32(s, uint16_t(o + 12)));
            std::string up;
            for (char ch : nd.element) up += char(std::toupper(static_cast<unsigned char>(ch)));
            nd.video = up.find(".AVI") != std::string::npos ||
                       type.find("avi") != std::string::npos || type.find("AVI") != std::string::npos;
            if (!nd.video && !nd.element.empty() && !sound_load(nd.snd, nd.element)) {
                result = 275;   // MCIERR_FILE_NOT_FOUND
                break;
            }
            c.wr16(s, uint16_t(o + 4), nd.id);
            g_mci[nd.id] = std::move(nd);
            break;
        }
        case 0x0804:   // MCI_CLOSE
            if (d) { sound_stop(d->snd); g_mci.erase(dev); }
            break;
        case 0x0806:   // MCI_PLAY
            if (!d) { result = 257; break; }
            if (d->video) {
                recomp::log("MCI: video %s (playback not implemented yet)", d->element.c_str());
            } else {
                sound_play(d->snd, false);
            }
            set_notify();
            if (flags & 2) {   // MCI_WAIT
                while (!d->video && sound_busy(d->snd)) host_pump(10);
            }
            break;
        case 0x0808: case 0x0809:   // MCI_STOP, MCI_PAUSE
            if (d) {
                if (d->notifyPending) {
                    d->notifyPending = false;
                    post_message(d->notifyHwnd, MM_MCINOTIFY, 4, dev);   // MCI_NOTIFY_ABORTED
                }
                sound_stop(d->snd);
                if (!d->video && !d->element.empty()) sound_load(d->snd, d->element);
            }
            set_notify();
            break;
        case 0x0807: case 0x0830: case 0x0855: case 0x080D: case 0x0841: case 0x0842: case 0x0843:
            set_notify();   // SEEK, CUE, RESUME, SET, WINDOW, PUT, WHERE: accepted
            if (msg == 0x0843 && parms) {   // MCI_WHERE: report a 320x240 rectangle
                c.wr16(s, uint16_t(o + 4), 0); c.wr16(s, uint16_t(o + 6), 0);
                c.wr16(s, uint16_t(o + 8), 320); c.wr16(s, uint16_t(o + 10), 240);
            }
            break;
        case 0x0814: {   // MCI_STATUS
            uint32_t item = c.rd32(s, uint16_t(o + 8));
            uint32_t v = 0;
            switch (item) {
                case 1: v = d ? (d->video ? 1 : d->snd.lengthMs) : 0; break;   // LENGTH
                case 2: v = 0; break;                                           // POSITION
                case 3: v = 1; break;                                           // NUMBER_OF_TRACKS
                case 4: v = (d && !d->video && sound_busy(d->snd)) ? 526 : 525; break;   // MODE play/stop
                case 5: case 7: v = 1; break;                                   // MEDIA_PRESENT, READY
                case 6: v = 0; break;                                           // TIME_FORMAT ms
                default: v = 0; break;
            }
            c.wr32(s, uint16_t(o + 4), v);
            break;
        }
        case 0x080B: c.wr32(s, uint16_t(o + 4), 1); break;   // GETDEVCAPS: TRUE
        case 0x080A: c.wr8(s, uint16_t(c.rd32(s, uint16_t(o + 4))), 0); break;   // INFO: empty
        default:
            recomp::log("MCI command %04X not handled", msg);
            break;
    }
    a.ret32(result);
}

API(MMSYSTEM, 706, mciGetErrorString, 10) {
    uint16_t n = a.w(); uint32_t buf = a.p(); a.l();
    recomp::write_cstr(c, buf, "MCI error", n);
    a.ret(1);
}
