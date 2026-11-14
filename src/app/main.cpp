// Myulk recomp — skeleton app: a viewer for the original game's forms.
//
// Controls: Left/Right or PageUp/PageDown switch form, F1 toggles component
// outlines, F11 toggles fullscreen, Esc quits. Hovering shows the component
// under the mouse and its event handlers; clicking logs the handler that
// the original game would run.
//
// Usage: myulk [FORMNAME] [--outlines] [--screenshot out.bmp]
//
// Uses SDL3's callback entry points so the same code runs on desktop and in
// the browser (Emscripten drives SDL_AppIterate from the browser's frame loop).
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/assets.hpp"
#include "app/form_view.hpp"
#include "core/calendar.hpp"
#include "core/form.hpp"

namespace fs = std::filesystem;
using namespace myulk;

namespace {

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<TextureCache> textures;
    std::unique_ptr<FormView> view;
    std::vector<std::string> formFiles;
    int current = 0;
    Form form;
    FormViewOptions opt;
    const Component* hover = nullptr;
    std::string screenshot;   // --screenshot <file.bmp>: render once, save, exit

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

void drawStatus(App& app) {
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

}  // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    SDL_SetAppMetadata("Myulk", "0.1.0", "org.myulk.recomp");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    auto* app = new App;
    *appstate = app;

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

    for (const auto& e : fs::directory_iterator(fs::path(*root) / "MYULK" / "forms"))
        if (e.path().extension() == ".json") app->formFiles.push_back(e.path().string());
    std::sort(app->formFiles.begin(), app->formFiles.end());

    std::string start = "TFORM1";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) app->screenshot = argv[++i];
        else if (a == "--outlines") app->opt.outlines = true;
        else start = a;
    }
    int idx = 0;
    for (size_t i = 0; i < app->formFiles.size(); ++i)
        if (fs::path(app->formFiles[i]).stem().string() == start) idx = int(i);
    app->loadForm(idx);

    SDL_Log("calendar check: day %d = year %d, month %d, day %d", Calendar::kNewGameDay,
            Calendar::year(Calendar::kNewGameDay), Calendar::month0(Calendar::kNewGameDay) + 1,
            Calendar::dayOfMonth(Calendar::kNewGameDay));
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* app = static_cast<App*>(appstate);
    SDL_ConvertEventToRenderCoordinates(app->renderer, event);
    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;
        case SDL_EVENT_KEY_DOWN:
            switch (event->key.key) {
                case SDLK_ESCAPE: return SDL_APP_SUCCESS;
                case SDLK_RIGHT: case SDLK_PAGEDOWN: app->loadForm(app->current + 1); break;
                case SDLK_LEFT: case SDLK_PAGEUP: app->loadForm(app->current - 1); break;
                case SDLK_F1: app->opt.outlines = !app->opt.outlines; break;
                case SDLK_F11: {
                    bool fs = SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(app->window, !fs);
                    break;
                }
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
    app->view->draw(app->form, app->opt);
    drawStatus(*app);
    if (!app->screenshot.empty()) {
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
    app->textures.reset();
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window) SDL_DestroyWindow(app->window);
    delete app;
}
