// Myulk recomp — entry point.
//
// Usage:
//   myulk                         start the game (adventure intro)
//   myulk --viewer [FORMNAME]     form viewer for the original screens
//   options: --outlines, --screenshot out.bmp (render one frame, save, exit),
//            --frames N (with --screenshot: run N frames first),
//            --click X,Y (screen coordinates, before the screenshot; repeatable)
//
// Game keys: F11 fullscreen, Esc quit.
// Viewer keys: Left/Right or PgUp/PgDn switch form, F1 outlines.
//
// Uses SDL3's callback entry points so the same code runs on desktop and in
// the browser (Emscripten drives SDL_AppIterate from the browser's frame loop).
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/adventure_screen.hpp"
#include "app/assets.hpp"
#include "app/audio.hpp"
#include "app/cursors.hpp"
#include "app/form_view.hpp"
#include "core/form.hpp"

namespace fs = std::filesystem;
using namespace myulk;

namespace {

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<TextureCache> textures;
    std::unique_ptr<FormView> view;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<Cursors> cursors;
    std::unique_ptr<AdventureScreen> adventure;
    Uint64 lastTicks = 0;

    // --screenshot automation
    std::string screenshot;
    int framesBeforeShot = 1;
    std::vector<std::pair<int, int>> clicks;
    int frame = 0;

    // viewer mode
    bool viewer = false;
    std::vector<std::string> formFiles;
    int current = 0;
    Form form;
    FormViewOptions opt;
    const Component* hover = nullptr;

    void loadForm(int index) {
        if (formFiles.empty()) return;
        current = (index + int(formFiles.size())) % int(formFiles.size());
        try {
            form = Form::load(formFiles[current]);
        } catch (const std::exception& e) {
            SDL_Log("%s", e.what());
            form = Form{};
            form.root = std::make_unique<Component>();
        }
        hover = nullptr;
        std::string title = "Myulk - " + fs::path(formFiles[current]).stem().string() + " (" +
                            std::to_string(current + 1) + "/" + std::to_string(formFiles.size()) + ")";
        SDL_SetWindowTitle(window, title.c_str());
    }
};

void drawViewerStatus(App& app) {
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 200);
    SDL_FRect bar{0, FormView::kScreenH - 12.0f, float(FormView::kScreenW), 12};
    SDL_RenderFillRect(app.renderer, &bar);
    std::string s = fs::path(app.formFiles.empty() ? "" : app.formFiles[app.current]).stem().string();
    if (app.hover) {
        s += "  " + app.hover->name + ":" + app.hover->cls;
        for (const auto& [ev, h] : app.hover->events) s += "  " + ev + "=" + h;
    }
    SDL_SetRenderDrawColor(app.renderer, 255, 255, 160, 255);
    SDL_RenderDebugText(app.renderer, 2, FormView::kScreenH - 10.0f, s.c_str());
}

// Synthesize a left click at screen coordinates (for --click automation).
void injectClick(App& app, int x, int y) {
    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.x = float(x);
    ev.motion.y = float(y);
    app.adventure->handleEvent(ev);
    ev = SDL_Event{};
    ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = float(x);
    ev.button.y = float(y);
    app.adventure->handleEvent(ev);
}

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("Myulk", "0.1.0", "org.myulk.recomp");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    auto* app = new App;
    *appstate = app;

    std::string startForm = "TFORM1";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--viewer") app->viewer = true;
        else if (a == "--screenshot" && i + 1 < argc) app->screenshot = argv[++i];
        else if (a == "--frames" && i + 1 < argc) app->framesBeforeShot = std::max(1, std::atoi(argv[++i]));
        else if (a == "--click" && i + 1 < argc) {
            std::string v = argv[++i];
            auto comma = v.find(',');
            if (comma != std::string::npos)
                app->clicks.emplace_back(std::atoi(v.c_str()), std::atoi(v.c_str() + comma + 1));
        } else if (a == "--outlines") app->opt.outlines = true;
        else startForm = a;
    }

    if (!SDL_CreateWindowAndRenderer("Myulk", FormView::kScreenW * 2, FormView::kScreenH * 2,
                                     SDL_WINDOW_RESIZABLE, &app->window, &app->renderer)) {
        SDL_Log("window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderLogicalPresentation(app->renderer, FormView::kScreenW, FormView::kScreenH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(app->renderer, 1);

    auto root = findDataRoot();
    if (!root) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Myulk",
            "Game data not found.\n\nRun the extraction tools (see docs/extraction.md) "
            "or set MYULK_DATA to the extracted/resources folder.", app->window);
        return SDL_APP_FAILURE;
    }
    SDL_Log("data: %s", root->c_str());
    app->textures = std::make_unique<TextureCache>(app->renderer, *root);
    app->view = std::make_unique<FormView>(app->renderer, *app->textures);

    if (app->viewer) {
        for (const auto& e : fs::directory_iterator(fs::path(*root) / "MYULK" / "forms"))
            if (e.path().extension() == ".json") app->formFiles.push_back(e.path().string());
        std::sort(app->formFiles.begin(), app->formFiles.end());
        int idx = 0;
        for (size_t i = 0; i < app->formFiles.size(); ++i)
            if (fs::path(app->formFiles[i]).stem().string() == startForm) idx = int(i);
        app->loadForm(idx);
    } else {
        std::string iso = (fs::path(*root).parent_path() / "iso").string();
        app->audio = std::make_unique<Audio>();
        app->cursors = std::make_unique<Cursors>(app->renderer,
                                                 (fs::path(*root) / "MYULK" / "cursors").string());
        app->adventure = std::make_unique<AdventureScreen>(app->renderer, *app->textures, *app->view,
                                                           *app->audio, *app->cursors, iso);
        SDL_HideCursor();
        app->adventure->start();
    }
    app->lastTicks = SDL_GetTicks();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* app = static_cast<App*>(appstate);
    SDL_ConvertEventToRenderCoordinates(app->renderer, event);
    if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
    if (event->type == SDL_EVENT_KEY_DOWN) {
        switch (event->key.key) {
            case SDLK_ESCAPE: return SDL_APP_SUCCESS;
            case SDLK_F11: {
                bool full = SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(app->window, !full);
                return SDL_APP_CONTINUE;
            }
            default: break;
        }
    }
    if (app->adventure) {
        app->adventure->handleEvent(*event);
        return SDL_APP_CONTINUE;
    }
    switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
            switch (event->key.key) {
                case SDLK_RIGHT: case SDLK_PAGEDOWN: app->loadForm(app->current + 1); break;
                case SDLK_LEFT: case SDLK_PAGEUP: app->loadForm(app->current - 1); break;
                case SDLK_F1: app->opt.outlines = !app->opt.outlines; break;
                default: break;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            app->hover = app->view->hitTest(app->form, event->motion.x, event->motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (auto* c = app->view->hitTest(app->form, event->button.x, event->button.y)) {
                for (const char* ev : {"OnClick", "OnMouseDown", "OnEndDrag"}) {
                    if (auto it = c->events.find(ev); it != c->events.end()) {
                        SDL_Log("%s.%s -> %s", c->name.c_str(), ev, it->second.c_str());
                        break;
                    }
                }
            }
            break;
        default:
            break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto* app = static_cast<App*>(appstate);
    Uint64 now = SDL_GetTicks();
    int elapsed = int(now - app->lastTicks);
    app->lastTicks = now;
    ++app->frame;

    if (app->adventure) {
        // automation: one queued click every 3 frames, so deferred callbacks run in between
        if (!app->clicks.empty() && app->frame % 3 == 0) {
            auto [x, y] = app->clicks.front();
            app->clicks.erase(app->clicks.begin());
            injectClick(*app, x, y);
        }
        app->adventure->update(elapsed);
        app->adventure->draw();
    } else {
        app->view->draw(app->form, app->opt);
        drawViewerStatus(*app);
    }

    if (!app->screenshot.empty() && app->clicks.empty() && app->frame >= app->framesBeforeShot) {
        SDL_Surface* shot = SDL_RenderReadPixels(app->renderer, nullptr);
        bool ok = shot && SDL_SaveBMP(shot, app->screenshot.c_str());
        if (shot) SDL_DestroySurface(shot);
        SDL_Log("screenshot %s: %s", app->screenshot.c_str(), ok ? "saved" : SDL_GetError());
        return ok ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    SDL_RenderPresent(app->renderer);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    auto* app = static_cast<App*>(appstate);
    if (!app) return;
    app->adventure.reset();
    app->cursors.reset();
    app->audio.reset();
    app->textures.reset();
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    delete app;
}
