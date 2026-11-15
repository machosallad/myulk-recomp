// MMSYSTEM: sndPlaySound and the MCI subset used by Delphi's TMediaPlayer:
// waveaudio through SDL, and AVI movies (Cinepak + PCM, see avi.cpp) drawn
// into the device's display window at the MCI_PUT rectangle.
#include <SDL3/SDL.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/avi.hpp"
#include "recomp/win16/user.hpp"
#include "recomp/win16/vfs.hpp"
#include "recomp/win16/win16.hpp"

using recomp::Args;
using recomp::Cpu;

namespace win16 {
namespace {

SDL_AudioDeviceID g_dev = 0;

SDL_AudioDeviceID audio_device() {
    if (!g_dev) g_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    return g_dev;
}

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
    s.stream = SDL_CreateAudioStream(&spec, &spec);
    if (!s.stream || !audio_device() || !SDL_BindAudioStream(g_dev, s.stream)) return false;
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

// ---- AVI devices ------------------------------------------------------------

struct Video {
    Avi avi;
    uint16_t hwnd = 0;             // display window (0: none given)
    Rect dest;                     // left, top, width, height (0 size: natural)
    bool playing = false;
    uint32_t posMs = 0;            // position when not playing / at play start
    uint32_t toMs = 0;             // play end
    uint64_t startTick = 0;
    SDL_AudioStream* audio = nullptr;
    bool visible = true;

    uint32_t position() const {
        if (!playing) return posMs;
        return std::min<uint32_t>(toMs, posMs + uint32_t(SDL_GetTicks() - startTick));
    }
    void stop_audio() {
        if (audio) SDL_DestroyAudioStream(audio);
        audio = nullptr;
    }
};

struct McDevice {
    uint16_t id = 0;
    std::string element;
    Sound snd;
    std::unique_ptr<Video> video;
    uint16_t notifyHwnd = 0;
    bool notifyPending = false;
};
std::map<uint16_t, McDevice> g_mci;
uint16_t g_nextMci = 1;

void video_draw(Video& v) {
    if (!v.visible || v.avi.decoded() < 0) return;
    int ox = 0, oy = 0;
    Rect clip{0, 0, 640, 480};
    if (v.hwnd) {
        screen_to_client(v.hwnd, &ox, &oy);
        ox = -ox;
        oy = -oy;
        clip = wnd_visible_rect(v.hwnd);
    }
    int w = v.dest.right > 0 ? v.dest.right : v.avi.width;
    int h = v.dest.bottom > 0 ? v.dest.bottom : v.avi.height;
    int x0 = ox + v.dest.left, y0 = oy + v.dest.top;
    Surface& scr = screen();
    const auto& pic = v.avi.picture();
    Rect r = clip.intersect(Rect{x0, y0, x0 + w, y0 + h}).intersect(Rect{0, 0, scr.w, scr.h});
    std::vector<Rect> above = v.hwnd ? wnd_occluders(v.hwnd) : std::vector<Rect>{};
    for (const Rect& o : above)
        if (o.left <= r.left && o.top <= r.top && o.right >= r.right && o.bottom >= r.bottom) return;   // hidden
    for (int y = r.top; y < r.bottom; ++y) {
        int sy = (y - y0) * v.avi.height / h;
        for (int x = r.left; x < r.right; ++x) {
            bool covered = false;
            for (const Rect& o : above) covered = covered || o.contains(x, y);
            if (covered) continue;
            int sx = (x - x0) * v.avi.width / w;
            scr.set(x, y, pic[size_t(sy) * v.avi.width + sx]);
        }
    }
}

void video_play(Video& v, uint32_t from, uint32_t to) {
    v.posMs = std::min(from, v.avi.lengthMs());
    v.toMs = std::min(to, v.avi.lengthMs());
    // testing aid: MYULK_SKIP_MOVIES=1 skips every movie, =ms only those started before then
    static const char* skipEnv = SDL_getenv("MYULK_SKIP_MOVIES");
    static const uint64_t skipUntil = skipEnv ? (std::atoi(skipEnv) <= 1 ? UINT64_MAX : uint64_t(std::atoi(skipEnv))) : 0;
    if (SDL_GetTicks() < skipUntil) v.posMs = v.toMs;
    v.startTick = SDL_GetTicks();
    v.playing = v.posMs < v.toMs;
    v.stop_audio();
    if (v.playing && !v.avi.audio.empty() && v.avi.audioRate > 0) {
        SDL_AudioSpec spec{v.avi.audioBits == 16 ? SDL_AUDIO_S16LE : SDL_AUDIO_U8, v.avi.audioChannels,
                           v.avi.audioRate};
        int bps = (v.avi.audioBits / 8) * v.avi.audioChannels;
        size_t a = size_t(uint64_t(v.posMs) * v.avi.audioRate / 1000) * bps;
        size_t b = std::min(v.avi.audio.size(), size_t(uint64_t(v.toMs) * v.avi.audioRate / 1000) * bps);
        v.audio = SDL_CreateAudioStream(&spec, &spec);
        if (v.audio && audio_device() && SDL_BindAudioStream(g_dev, v.audio) && a < b) {
            SDL_PutAudioStreamData(v.audio, v.avi.audio.data() + a, int(b - a));
            SDL_FlushAudioStream(v.audio);
        }
    }
}

// Advance a playing movie; returns true while it is still playing.
bool video_poll(Video& v) {
    if (!v.playing) return false;
    uint32_t pos = v.position();
    int frame = int(uint64_t(pos) * v.avi.fps / 1000.0);
    if (frame != v.avi.decoded()) v.avi.decode_to(frame);
    video_draw(v);
    if (pos >= v.toMs) {
        v.posMs = v.toMs;
        v.playing = false;
        return false;
    }
    return true;
}

void post_notify(McDevice& d, uint16_t code) {
    if (!d.notifyPending) return;
    d.notifyPending = false;
    post_message(d.notifyHwnd, MM_MCINOTIFY, code, d.id);
}

}  // namespace

// Called while waiting for messages: advance media and post MCI notifications.
void mm_poll() {
    sound_busy(g_snd);
    for (auto& [id, d] : g_mci) {
        bool busy = d.video ? video_poll(*d.video) : sound_busy(d.snd);
        if (!busy) post_notify(d, 1);   // MCI_NOTIFY_SUCCESSFUL
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
    Video* v = d ? d->video.get() : nullptr;
    auto set_notify = [&]() {
        if ((flags & 1) && d && parms) {   // MCI_NOTIFY
            if (d->notifyPending) post_notify(*d, 2);   // superseded
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
            for (char ch : nd.element + "|" + type) up += char(std::toupper(static_cast<unsigned char>(ch)));
            bool isVideo = up.find(".AVI") != std::string::npos || up.find("AVIVIDEO") != std::string::npos;
            if (isVideo) {
                auto vid = std::make_unique<Video>();
                std::string host = nd.element.empty() ? std::string() : vfs::to_host(nd.element);
                if (!nd.element.empty() && (host.empty() || !vid->avi.open(host))) {
                    recomp::log("MCI: cannot open movie %s", nd.element.c_str());
                    result = 275;   // MCIERR_FILE_NOT_FOUND
                    break;
                }
                nd.video = std::move(vid);
            } else if (!nd.element.empty() && !sound_load(nd.snd, nd.element)) {
                result = 275;
                break;
            }
            c.wr16(s, uint16_t(o + 4), nd.id);
            g_mci[nd.id] = std::move(nd);
            break;
        }
        case 0x0804:   // MCI_CLOSE
            if (d) {
                post_notify(*d, 4);
                sound_stop(d->snd);
                if (v) {
                    v->stop_audio();
                    if (v->hwnd) wnd_invalidate(v->hwnd);
                }
                g_mci.erase(dev);
            }
            break;
        case 0x0806: {   // MCI_PLAY: dwCallback, dwFrom(+4), dwTo(+8)
            if (!d) { result = 257; break; }
            set_notify();
            if (v) {
                uint32_t from = (flags & 4) ? c.rd32(s, uint16_t(o + 4)) : v->position();
                uint32_t to = (flags & 8) ? c.rd32(s, uint16_t(o + 8)) : v->avi.lengthMs();
                if (from >= v->avi.lengthMs()) from = v->avi.lengthMs();
                video_play(*v, from, to);
                video_poll(*v);
                if (flags & 2) while (video_poll(*v)) host_pump(10);   // MCI_WAIT
            } else {
                sound_play(d->snd, false);
                if (flags & 2) while (sound_busy(d->snd)) host_pump(10);
            }
            break;
        }
        case 0x0808: case 0x0809:   // MCI_STOP, MCI_PAUSE
            if (d) {
                post_notify(*d, 4);   // MCI_NOTIFY_ABORTED
                if (v) {
                    v->posMs = v->position();
                    v->playing = false;
                    v->stop_audio();
                } else {
                    sound_stop(d->snd);
                    if (!d->element.empty()) sound_load(d->snd, d->element);
                }
            }
            set_notify();
            break;
        case 0x0855:   // MCI_RESUME
            if (v && !v->playing) video_play(*v, v->posMs, v->toMs ? v->toMs : v->avi.lengthMs());
            set_notify();
            break;
        case 0x0807: {   // MCI_SEEK: TO_START 0x100, TO_END 0x200, MCI_TO 8 (dwTo at +4)
            if (v) {
                v->playing = false;
                v->stop_audio();
                if (flags & 0x100) v->posMs = 0;
                else if (flags & 0x200) v->posMs = v->avi.lengthMs();
                else if (flags & 8) v->posMs = std::min(c.rd32(s, uint16_t(o + 4)), v->avi.lengthMs());
                int frame = int(uint64_t(v->posMs) * v->avi.fps / 1000.0);
                v->avi.decode_to(std::min(frame, v->avi.frameCount() - 1));
            }
            set_notify();
            break;
        }
        case 0x0841:   // MCI_WINDOW: dwCallback, hWnd(+4), wReserved, nCmdShow(+8)
            if (v) {
                if (flags & 0x10000) v->hwnd = c.rd16(s, uint16_t(o + 4));             // MCI_ANIM_WINDOW_HWND
                if (flags & 0x40000) v->visible = c.rd16(s, uint16_t(o + 8)) != 0;     // MCI_ANIM_WINDOW_STATE
            }
            set_notify();
            break;
        case 0x0842:   // MCI_PUT: dwCallback, rc(+4) = left, top, width, height
            if (v && (flags & 0x10000) && !(flags & 0x20000)) {   // MCI_ANIM_RECT, not SOURCE
                v->dest = Rect{int16_t(c.rd16(s, uint16_t(o + 4))), int16_t(c.rd16(s, uint16_t(o + 6))),
                               int16_t(c.rd16(s, uint16_t(o + 8))), int16_t(c.rd16(s, uint16_t(o + 10)))};
                if (v->hwnd) wnd_invalidate(v->hwnd);
            }
            set_notify();
            break;
        case 0x0843:   // MCI_WHERE: rectangle (left, top, width, height)
            if (parms) {
                Rect r{0, 0, 320, 240};
                if (v) {
                    bool src = (flags & 0x20000) != 0;
                    r = Rect{src ? 0 : v->dest.left, src ? 0 : v->dest.top,
                             !src && v->dest.right > 0 ? v->dest.right : v->avi.width,
                             !src && v->dest.bottom > 0 ? v->dest.bottom : v->avi.height};
                }
                c.wr16(s, uint16_t(o + 4), uint16_t(r.left)); c.wr16(s, uint16_t(o + 6), uint16_t(r.top));
                c.wr16(s, uint16_t(o + 8), uint16_t(r.right)); c.wr16(s, uint16_t(o + 10), uint16_t(r.bottom));
            }
            set_notify();
            break;
        case 0x0830: case 0x080D:   // MCI_CUE, MCI_SET (time format stays milliseconds)
            set_notify();
            break;
        case 0x0840:   // MCI_REALIZE (palette): nothing to do with a true-colour screen
            set_notify();
            break;
        case 0x0814: {   // MCI_STATUS
            uint32_t item = c.rd32(s, uint16_t(o + 8));
            uint32_t val = 0;
            switch (item) {
                case 1: val = v ? v->avi.lengthMs() : (d ? d->snd.lengthMs : 0); break;    // LENGTH
                case 2: val = v ? v->position() : 0; break;                                // POSITION
                case 3: val = 1; break;                                                    // NUMBER_OF_TRACKS
                case 4:                                                                    // MODE
                    val = (v ? v->playing : (d && sound_busy(d->snd))) ? 526 : 525;        // playing / stopped
                    break;
                case 5: case 7: val = 1; break;                                            // MEDIA_PRESENT, READY
                case 6: val = 0; break;                                                    // TIME_FORMAT: ms
                case 8: val = 0; break;                                                    // CURRENT_TRACK
                default: val = 0; break;
            }
            c.wr32(s, uint16_t(o + 4), val);
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
