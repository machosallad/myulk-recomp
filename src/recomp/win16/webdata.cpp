#include "recomp/win16/webdata.hpp"

#include <map>

#include "recomp/runtime/runtime.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

// Text of a URL (malloc'ed, caller frees), or null.
EM_ASYNC_JS(char*, web_fetch_text, (const char* url), {
    try {
        const r = await fetch(UTF8ToString(url));
        if (!r.ok) return 0;
        return stringToNewUTF8(await r.text());
    } catch (e) {
        console.warn('fetch failed', e);
        return 0;
    }
});

// Download a URL into a file of the in-memory file system; 1 on success.
// Files are kept in Cache Storage ("myulk-game-<key>", key from the manifest)
// so later visits read them locally; without Cache Storage (no secure
// context) it is a plain fetch.
EM_ASYNC_JS(int, web_fetch_file, (const char* url, const char* path, const char* cacheKey), {
    const u = UTF8ToString(url), p = UTF8ToString(path), key = UTF8ToString(cacheKey);
    let cache = null;
    try {
        if (typeof caches !== 'undefined') cache = await caches.open('myulk-game-' + key);
    } catch (e) { cache = null; }
    try {
        let r = cache ? await cache.match(u) : null;
        if (!r) {
            if (Module.onGameFetch) Module.onGameFetch(true, u);
            try {
                r = await fetch(u);
            } finally {
                if (Module.onGameFetch) Module.onGameFetch(false, u);
            }
            if (!r.ok) { console.warn('missing game file', u); return 0; }
            if (cache) {
                try { await cache.put(u, r.clone()); } catch (e) { console.warn('caching failed', u, e); }
            }
        }
        FS.writeFile(p, new Uint8Array(await r.arrayBuffer()));
        return 1;
    } catch (e) {
        console.warn('fetch failed', u, e);
        return 0;
    }
});

// Keep the cached game files from being evicted (browsers decide whether to
// grant it) and drop caches of other manifest versions.
EM_JS(void, web_cache_setup, (const char* cacheKey), {
    const key = 'myulk-game-' + UTF8ToString(cacheKey);
    if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
    if (typeof caches !== 'undefined')
        caches.keys().then((names) => names.forEach((n) => {
            if (n.startsWith('myulk-game-') && n !== key) caches.delete(n);
        })).catch(() => {});
});
#endif

namespace win16::webdata {
namespace {
std::string g_root;
std::map<std::string, long> g_pending;   // host path -> size, not fetched yet
std::string g_cacheKey;                  // hash of the manifest: names the browser cache
}

#ifdef __EMSCRIPTEN__
bool init(const std::string& root) {
    namespace fs = std::filesystem;
    g_root = root;
    char* text = web_fetch_text("game/manifest.txt");
    if (!text) {
        recomp::log("web data: game/manifest.txt not found");
        return false;
    }
    std::string all = text;
    std::free(text);
    uint32_t h = 2166136261u;   // FNV-1a
    for (unsigned char ch : all) h = (h ^ ch) * 16777619u;
    char key[16];
    std::snprintf(key, sizeof key, "%08x", h);
    g_cacheKey = key;
    web_cache_setup(g_cacheKey.c_str());
    std::error_code ec;
    size_t p = 0, count = 0;
    while (p < all.size()) {
        size_t nl = all.find('\n', p);
        std::string line = all.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = nl == std::string::npos ? all.size() : nl + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string host = root + "/" + line.substr(0, tab);
        fs::create_directories(fs::path(host).parent_path(), ec);
        if (std::FILE* f = std::fopen(host.c_str(), "wb")) std::fclose(f);   // empty placeholder
        g_pending[host] = std::atol(line.c_str() + tab + 1);
        ++count;
    }
    recomp::log("web data: %zu files available on demand", count);
    return true;
}

void ensure(const std::string& hostPath) {
    auto it = g_pending.find(hostPath);
    if (it == g_pending.end()) return;
    std::string url = "game/" + hostPath.substr(g_root.size() + 1);
    if (web_fetch_file(url.c_str(), hostPath.c_str(), g_cacheKey.c_str())) g_pending.erase(it);
}

long pending_size(const std::string& hostPath) {
    auto it = g_pending.find(hostPath);
    return it == g_pending.end() ? -1 : it->second;
}
#else
bool init(const std::string& root) { g_root = root; return true; }
void ensure(const std::string&) {}
long pending_size(const std::string&) { return -1; }
#endif

}  // namespace win16::webdata
