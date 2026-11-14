#include "recomp/win16/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

#include "recomp/runtime/runtime.hpp"

namespace fs = std::filesystem;

namespace win16::vfs {
namespace {

std::string g_root;         // host folder for C:\ .
std::string g_cwd = "C:\\";
std::map<int, std::FILE*> g_files;
int g_next = 5;

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

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
    if (fs::exists(dir / name, ec)) return name;
    std::string want = upper(name);
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (upper(n) == want) return n;
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

std::string to_host(const std::string& dosPath, bool forCreate) {
    std::string n = normalize(dosPath);
    fs::path cur(g_root);
    auto parts = split(n.substr(2));
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string m = match(cur, parts[i]);
        if (m.empty()) {
            if (forCreate && i + 1 == parts.size()) return (cur / parts[i]).string();
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
    const char* m = (mode & 3) == 0 ? "rb" : (mode & 3) == 1 ? "r+b" : "r+b";
    std::FILE* f = std::fopen(h.c_str(), m);
    if (!f) {
        *error = 5;
        return -1;
    }
    int id = g_next++;
    g_files[id] = f;
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
    return id;
}

bool close(int h) {
    auto it = g_files.find(h);
    if (it == g_files.end()) return h < 5;
    std::fclose(it->second);
    g_files.erase(it);
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
    std::string h = to_host(dosPath);
    std::error_code ec;
    return !h.empty() && fs::remove(h, ec);
}

bool rename(const std::string& from, const std::string& to) {
    std::string a = to_host(from), b = to_host(to, true);
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
    std::string h = to_host(dir);
    if (h.empty()) return out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(h, ec)) {
        std::string name = upper(e.path().filename().string());
        bool isdir = e.is_directory(ec);
        if (isdir && !(attrMask & 0x10)) continue;
        if (!wildmatch(pat, name)) continue;
        FindEntry fe;
        fe.name = name;
        fe.dir = isdir;
        fe.size = isdir ? 0 : uint32_t(e.file_size(ec));
        out.push_back(fe);
    }
    std::sort(out.begin(), out.end(), [](const FindEntry& a, const FindEntry& b) { return a.name < b.name; });
    return out;
}

}  // namespace win16::vfs
