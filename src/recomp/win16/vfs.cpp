#include "recomp/win16/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

#include "recomp/runtime/runtime.hpp"
#include "recomp/win16/webdata.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace fs = std::filesystem;

namespace win16::vfs {
namespace {

std::string g_root;         // host folder for C:\ .
std::string g_cwd = "C:\\";
std::map<int, std::FILE*> g_files;
std::set<int> g_writable;   // handles opened for writing (saves to persist on close)
int g_next = 5;

// DOS-side names are Windows-1252 bytes, upper-cased like Windows does
// (including å ä ö é ...).
std::string upper(std::string s) {
    for (char& c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'a' && u <= 'z') c = char(u - 32);
        else if (u >= 0xE0 && u <= 0xFE && u != 0xF7) c = char(u - 32);
    }
    return s;
}

// Host file names: Windows uses the same 8-bit code page as the game; other
// hosts (the web build, Linux, macOS) use UTF-8, so names are converted.
#if defined(_WIN32)
std::string host_name(const std::string& dos) { return dos; }
std::string dos_name(const std::string& host) { return host; }
#else
std::string host_name(const std::string& dos) {
    std::string out;
    for (unsigned char b : dos) {
        if (b < 0x80) { out += char(b); continue; }
        out += char(0xC0 | (b >> 6));
        out += char(0x80 | (b & 0x3F));
    }
    return out;
}
std::string dos_name(const std::string& host) {
    std::string out;
    for (size_t i = 0; i < host.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(host[i]);
        if (b < 0x80) { out += char(b); continue; }
        if ((b & 0xE0) == 0xC0 && i + 1 < host.size()) {
            unsigned cp = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(host[i + 1]) & 0x3Fu);
            out += cp < 0x100 ? char(cp) : '?';
            ++i;
            continue;
        }
        out += '?';
        while (i + 1 < host.size() && (static_cast<unsigned char>(host[i + 1]) & 0xC0) == 0x80) ++i;
    }
    return out;
}
#endif

std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Find a directory entry by case-insensitive name.
std::string match(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (fs::exists(dir / host_name(name), ec)) return host_name(name);
    std::string want = upper(name);
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (upper(dos_name(n)) == want) return n;
    }
    return {};
}

bool wildmatch(const std::string& pat, const std::string& name) {
    // DOS 8.3 wildcard: compare name and extension parts separately
    auto splitext = [](const std::string& s) {
        auto dot = s.rfind('.');
        return dot == std::string::npos ? std::make_pair(s, std::string()) :
                                          std::make_pair(s.substr(0, dot), s.substr(dot + 1));
    };
    auto part = [](const std::string& p, const std::string& n) {
        size_t i = 0;
        for (; i < p.size(); ++i) {
            if (p[i] == '*') return true;
            if (i >= n.size()) return p[i] == '?' && false;
            if (p[i] != '?' && p[i] != n[i]) return false;
        }
        return i == n.size();
    };
    auto [pn, pe] = splitext(upper(pat));
    auto [nn, ne] = splitext(upper(name));
    if (pat.find('.') == std::string::npos) pe = "*";
    return part(pn, nn) && part(pe, ne);
}

}  // namespace

void init(const std::string& hostRoot, const std::string& gameSubdir) {
    g_root = hostRoot;
    g_cwd = "C:\\" + upper(gameSubdir);
}

std::string normalize(const std::string& dosPath) {
    std::string p = dosPath;
    std::vector<std::string> parts;
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
    else if (p.empty() || (p[0] != '\\' && p[0] != '/')) parts = split(g_cwd.substr(2));
    for (const auto& c : split(p)) {
        if (c == ".") continue;
        if (c == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(upper(c));
    }
    std::string out = "C:";
    for (const auto& c : parts) out += "\\" + c;
    if (parts.empty()) out += "\\";
    return out;
}

// Writable overlay: files the game creates or writes live under g_overlay
// (same DOS path, upper-case names); reads prefer the overlay.
std::string g_overlay;

fs::path overlay_path(const std::string& normalized) {
    fs::path p(g_overlay);
    for (const auto& c : split(normalized.substr(2))) p /= host_name(c);
    return p;
}

std::string base_host(const std::string& n, bool forCreate);

// Base (game data) path; on the web the file is fetched on first use.
std::string base_file(const std::string& n, bool forCreate) {
    std::string b = base_host(n, forCreate);
    if (!b.empty()) webdata::ensure(b);
    return b;
}

std::string to_host(const std::string& dosPath, bool forCreate) {
    std::string n = normalize(dosPath);
    if (g_overlay.empty()) return base_file(n, forCreate);
    std::error_code ec;
    fs::path o = overlay_path(n);
    if (!forCreate) {
        if (fs::exists(o, ec)) return o.string();
        return base_file(n, false);
    }
    // for writing: copy an existing base file into the overlay first
    fs::create_directories(o.parent_path(), ec);
    if (!fs::exists(o, ec)) {
        std::string b = base_file(n, false);
        if (!b.empty() && fs::is_regular_file(b, ec)) fs::copy_file(b, o, ec);
    }
    return o.string();
}

void set_overlay(const std::string& hostDir) { g_overlay = hostDir; }

std::string base_host(const std::string& n, bool forCreate) {
    fs::path cur(g_root);
    auto parts = split(n.substr(2));
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string m = match(cur, parts[i]);
        if (m.empty()) {
            if (forCreate && i + 1 == parts.size()) return (cur / host_name(parts[i])).string();
            return {};
        }
        cur /= m;
    }
    return cur.string();
}

std::string cwd() { return g_cwd; }

bool chdir(const std::string& dosPath) {
    std::string n = normalize(dosPath);
    std::error_code ec;
    std::string h = to_host(n);
    if (h.empty() || !fs::is_directory(h, ec)) return false;
    g_cwd = n;
    return true;
}

std::string exe_dos_path() { return g_cwd + "\\MYULK.EXE"; }

int open(const std::string& dosPath, int mode, int* error) {
    std::string h = to_host(dosPath);
    std::error_code ec;
    if (h.empty() || fs::is_directory(h, ec)) {
        *error = 2;
        return -1;
    }
    if ((mode & 3) != 0) h = to_host(dosPath, true);   // writes go to the overlay
    const char* m = (mode & 3) == 0 ? "rb" : (mode & 3) == 1 ? "r+b" : "r+b";
    std::FILE* f = std::fopen(h.c_str(), m);
    if (!f) {
        *error = 5;
        return -1;
    }
    int id = g_next++;
    g_files[id] = f;
    if ((mode & 3) != 0) g_writable.insert(id);
    return id;
}

int create(const std::string& dosPath, int* error) {
    std::string h = to_host(dosPath, true);
    if (h.empty()) {
        *error = 3;
        return -1;
    }
    std::FILE* f = std::fopen(h.c_str(), "w+b");
    if (!f) {
        *error = 5;
        return -1;
    }
    int id = g_next++;
    g_files[id] = f;
    g_writable.insert(id);
    return id;
}

bool close(int h) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return h < 5;
    std::fclose(it->second);
    g_files.erase(it);
    bool written = g_writable.erase(h) > 0;
#ifdef __EMSCRIPTEN__
    // persist saves (IndexedDB behind /saves) after a file opened for writing;
    // one sync at a time, a request during a sync runs once it is done
    if (written) {
        EM_ASM({
            if (typeof IDBFS === 'undefined') return;
            var run = function () {
                Module.__saveSync = 1;
                FS.syncfs(false, function (e) {
                    if (e) console.warn('saving failed', e);
                    var again = Module.__saveSync === 2;
                    Module.__saveSync = 0;
                    if (again) run();
                });
            };
            if (Module.__saveSync) Module.__saveSync = 2; else run();
        });
    }
#else
    (void)written;
#endif
    return true;
}

long read(int h, void* buf, long n) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return -1;
    return long(std::fread(buf, 1, size_t(n), it->second));
}

long write(int h, const void* buf, long n) {
    if (h == 1 || h == 2) return n;
    auto it = g_files.find(h);
    if (it == g_files.end()) return -1;
    if (n == 0) {   // DOS: writing 0 bytes truncates at the current position
        std::fflush(it->second);
        long pos = std::ftell(it->second);
        std::error_code ec;
        (void)pos;
        return 0;
    }
    return long(std::fwrite(buf, 1, size_t(n), it->second));
}

long seek(int h, long off, int whence) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return -1;
    if (std::fseek(it->second, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END)) return -1;
    return std::ftell(it->second);
}

long size(int h) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return -1;
    long cur = std::ftell(it->second);
    std::fseek(it->second, 0, SEEK_END);
    long s = std::ftell(it->second);
    std::fseek(it->second, cur, SEEK_SET);
    return s;
}

bool remove(const std::string& dosPath) {
    std::error_code ec;
    if (!g_overlay.empty()) return fs::remove(overlay_path(normalize(dosPath)), ec);   // never touch the game data
    std::string h = to_host(dosPath);
    return !h.empty() && fs::remove(h, ec);
}

bool rename(const std::string& from, const std::string& to) {
    if (to_host(from).empty()) return false;
    std::string a = to_host(from, true), b = to_host(to, true);
    std::error_code ec;
    if (a.empty() || b.empty()) return false;
    fs::rename(a, b, ec);
    return !ec;
}

std::vector<FindEntry> find(const std::string& dosPattern, int attrMask) {
    std::vector<FindEntry> out;
    std::string n = normalize(dosPattern);
    auto slash = n.rfind('\\');
    std::string dir = n.substr(0, slash), pat = n.substr(slash + 1);
    if (dir.size() == 2) dir += "\\";
    if (attrMask == 0x08) {   // volume label search: drive C: is the game CD
        FindEntry fe;
        fe.name = "MYULK";
        fe.label = true;
        out.push_back(fe);
        return out;
    }
    std::error_code ec;
    std::map<std::string, FindEntry> found;   // overlay entries replace base ones
    auto scan = [&](const std::string& h) {
        if (h.empty() || !fs::is_directory(h, ec)) return;
        for (const auto& e : fs::directory_iterator(h, ec)) {
            std::string name = upper(dos_name(e.path().filename().string()));
            bool isdir = e.is_directory(ec);
            if (isdir && !(attrMask & 0x10)) continue;
            if (!wildmatch(pat, name)) continue;
            FindEntry fe;
            fe.name = name;
            fe.dir = isdir;
            long pending = isdir ? -1 : webdata::pending_size(e.path().string());
            fe.size = isdir ? 0 : pending >= 0 ? uint32_t(pending) : uint32_t(e.file_size(ec));
            found[name] = fe;
        }
    };
    scan(base_host(normalize(dir), false));
    if (!g_overlay.empty()) scan(overlay_path(normalize(dir)).string());
    for (auto& [k, v] : found) out.push_back(v);
    return out;
}

}  // namespace win16::vfs
