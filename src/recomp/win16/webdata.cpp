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

// Text stored in a browser cache under a URL (malloc'ed), or null: the game
// files imported from the player's CD image live in the cache "myulk-own".
EM_ASYNC_JS(char*, web_cached_text, (const char* cacheName, const char* url), {
    try {
        if (typeof caches === 'undefined') return 0;
        const cache = await caches.open(UTF8ToString(cacheName));
        const r = await cache.match(UTF8ToString(url));
        return r ? stringToNewUTF8(await r.text()) : 0;
    } catch (e) {
        return 0;
    }
});

// Download a URL into a file of the in-memory file system; 1 on success.
// Files are kept in Cache Storage (`cacheName`: "myulk-game-<key>", key from
// the server's manifest, or "myulk-own" for imported files) so later visits
// read them locally; without Cache Storage (no secure context) it is a plain
// fetch.
EM_ASYNC_JS(int, web_fetch_file, (const char* url, const char* path, const char* cacheName), {
    const u = UTF8ToString(url), p = UTF8ToString(path), name = UTF8ToString(cacheName);
    let cache = null;
    try {
        if (typeof caches !== 'undefined') cache = await caches.open(name);
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
// grant it) and drop caches of other versions of the server's files (the
// imported files in "myulk-own" stay).
EM_JS(void, web_cache_setup, (const char* cacheName), {
    const key = UTF8ToString(cacheName);
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
std::string g_cacheName;                 // the browser cache holding the files
}

#ifdef __EMSCRIPTEN__
bool init(const std::string& root) {
    namespace fs = std::filesystem;
    g_root = root;
    // the server's game files, or the ones imported from the player's CD
    char* text = web_fetch_text("game/manifest.txt");
    bool own = false;
    if (!text) {
        text = web_cached_text("myulk-own", "game/manifest.txt");
        own = text != nullptr;
    }
    if (!text) {
        recomp::log("web data: no game files on the server or in this browser");
        return false;
    }
    std::string all = text;
    std::free(text);
    if (own) {
        g_cacheName = "myulk-own";
    } else {
        uint32_t h = 2166136261u;   // FNV-1a of the manifest names the cache
        for (unsigned char ch : all) h = (h ^ ch) * 16777619u;
        char key[24];
        std::snprintf(key, sizeof key, "myulk-game-%08x", h);
        g_cacheName = key;
    }
    web_cache_setup(g_cacheName.c_str());
    g_pending.clear();
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
    recomp::log("web data: %zu files available on demand (%s)", count, own ? "imported" : "server");
    return true;
}

void ensure(const std::string& hostPath) {
    auto it = g_pending.find(hostPath);
    if (it == g_pending.end()) return;
    std::string url = "game/" + hostPath.substr(g_root.size() + 1);
    if (web_fetch_file(url.c_str(), hostPath.c_str(), g_cacheName.c_str())) g_pending.erase(it);
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
