#include "stdlib/system_ops.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <unordered_map>

namespace fs = std::filesystem;

namespace transom::stdlib {

namespace {

fs::path storageFilePath(const std::string& appName) {
    std::string safeName = appName.empty() ? "default" : appName;
    for (char& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    }
#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    fs::path base = appData ? fs::path(appData) : fs::temp_directory_path();
    fs::path dir = base / "Transom" / safeName;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::temp_directory_path();
    fs::path dir = base / "Library" / "Application Support" / "Transom" / safeName;
#else
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::temp_directory_path();
    fs::path dir = base / ".config" / "Transom" / safeName;
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "store.kv";
}

std::unordered_map<std::string, std::string> loadStore(const fs::path& p) {
    std::unordered_map<std::string, std::string> map;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        size_t sep = line.find('\t');
        if (sep != std::string::npos) {
            map[line.substr(0, sep)] = line.substr(sep + 1);
        }
    }
    return map;
}

void saveStore(const fs::path& p, const std::unordered_map<std::string, std::string>& map) {
    std::ofstream ofs(p, std::ios::trunc);
    for (const auto& [k, v] : map) {
        std::string cleanV = v;
        for (char& c : cleanV) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        ofs << k << "\t" << cleanV << "\n";
    }
}

struct Semver {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
};

Semver parseSemver(const std::string& s) {
    Semver v;
    if (std::sscanf(s.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch) == 3) {
        v.valid = true;
    }
    return v;
}

int compareSemver(const Semver& a, const Semver& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

} // namespace

std::string fsRead(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool fsWrite(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << content;
    return static_cast<bool>(ofs);
}

bool fsExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string storageGet(const std::string& appName,
                       const std::string& key,
                       const std::string& fallback) {
    auto map = loadStore(storageFilePath(appName));
    auto it = map.find(key);
    return (it != map.end()) ? it->second : fallback;
}

void storageSet(const std::string& appName,
                const std::string& key,
                const std::string& value) {
    fs::path p = storageFilePath(appName);
    auto map = loadStore(p);
    map[key] = value;
    saveStore(p, map);
}

std::string jsonGet(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        std::string out;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                char esc = json[pos];
                if (esc == 'n') out.push_back('\n');
                else if (esc == 't') out.push_back('\t');
                else out.push_back(esc);
            } else {
                out.push_back(json[pos]);
            }
            pos++;
        }
        return out;
    }

    size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != ']' && !std::isspace(static_cast<unsigned char>(json[pos]))) {
        pos++;
    }
    std::string token = json.substr(start, pos - start);
    return (token == "null") ? "" : token;
}

namespace {

void skipJsonWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
}

size_t skipJsonValue(const std::string& s, size_t i) {
    skipJsonWs(s, i);
    if (i >= s.size()) return i;
    if (s[i] == '"') {
        i++;
        while (i < s.size()) {
            if (s[i] == '\\') i += 2;
            else if (s[i] == '"') return i + 1;
            else i++;
        }
        return i;
    }
    if (s[i] == '{' || s[i] == '[') {
        char open = s[i], close = (open == '{') ? '}' : ']';
        int depth = 1;
        i++;
        while (i < s.size() && depth > 0) {
            if (s[i] == '"') i = skipJsonValue(s, i);
            else {
                if (s[i] == open) depth++;
                else if (s[i] == close) depth--;
                i++;
            }
        }
        return i;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i]))) {
        i++;
    }
    return i;
}

std::string unquoteJson(const std::string& raw) {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 2 < raw.size()) {
                char c = raw[++i];
                if (c == 'n') out.push_back('\n');
                else if (c == 't') out.push_back('\t');
                else out.push_back(c);
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }
    return (raw == "null") ? "" : raw;
}

} // namespace

std::string jsonPath(const std::string& json, const std::string& dottedPath) {
    auto segments = strSplit(dottedPath, ".");
    std::string cur = strTrim(json);

    for (const auto& seg : segments) {
        if (cur.empty()) return "";
        size_t i = 0;
        skipJsonWs(cur, i);
        if (i >= cur.size()) return "";

        if (cur[i] == '[') {
            int targetIdx = -1;
            try { targetIdx = std::stoi(seg); } catch (...) { return ""; }
            if (targetIdx < 0) return "";
            i++; // '['
            int idx = 0;
            bool found = false;
            while (i < cur.size()) {
                skipJsonWs(cur, i);
                if (i >= cur.size() || cur[i] == ']') break;
                size_t start = i;
                size_t end = skipJsonValue(cur, i);
                if (idx == targetIdx) {
                    cur = cur.substr(start, end - start);
                    found = true;
                    break;
                }
                i = end;
                skipJsonWs(cur, i);
                if (i < cur.size() && cur[i] == ',') i++;
                idx++;
            }
            if (!found) return "";
        } else if (cur[i] == '{') {
            i++; // '{'
            bool found = false;
            while (i < cur.size()) {
                skipJsonWs(cur, i);
                if (i >= cur.size() || cur[i] == '}') break;
                size_t kStart = i;
                size_t kEnd = skipJsonValue(cur, i);
                std::string key = unquoteJson(cur.substr(kStart, kEnd - kStart));
                i = kEnd;
                skipJsonWs(cur, i);
                if (i < cur.size() && cur[i] == ':') i++;
                skipJsonWs(cur, i);
                size_t vStart = i;
                size_t vEnd = skipJsonValue(cur, i);
                if (key == seg) {
                    cur = cur.substr(vStart, vEnd - vStart);
                    found = true;
                    break;
                }
                i = vEnd;
                skipJsonWs(cur, i);
                if (i < cur.size() && cur[i] == ',') i++;
            }
            if (!found) return "";
        } else {
            return "";
        }
    }
    return unquoteJson(strTrim(cur));
}

// ---------------------------------------------------------------------------
// First-class Bytes operations
// ---------------------------------------------------------------------------
ValueBytes bytesRead(const std::string& path) {
    auto out = std::make_shared<std::vector<uint8_t>>();
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return out;
    auto size = ifs.tellg();
    if (size <= 0) return out;
    ifs.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    ifs.read(reinterpret_cast<char*>(out->data()), size);
    return out;
}

ValueBytes bytesSlice(const ValueBytes& b, int64_t start, int64_t len) {
    auto out = std::make_shared<std::vector<uint8_t>>();
    if (!b || start < 0 || len <= 0 || static_cast<size_t>(start) >= b->size()) return out;
    size_t s = static_cast<size_t>(start);
    size_t n = std::min(static_cast<size_t>(len), b->size() - s);
    out->assign(b->begin() + s, b->begin() + s + n);
    return out;
}

std::string bytesToString(const ValueBytes& b) {
    if (!b || b->empty()) return "";
    return std::string(reinterpret_cast<const char*>(b->data()), b->size());
}

std::string bytesToHex(const ValueBytes& b) {
    if (!b || b->empty()) return "";
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(b->size() * 2);
    for (uint8_t v : *b) {
        out.push_back(kHex[(v >> 4) & 0xF]);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------
std::string strTrim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> strSplit(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    if (sep.empty()) {
        out.push_back(s);
        return out;
    }
    size_t pos = 0;
    while (true) {
        size_t next = s.find(sep, pos);
        if (next == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, next - pos));
        pos = next + sep.size();
    }
    return out;
}

bool strContains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

bool strStartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string strReplace(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out = s;
    size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
    }
    return out;
}

std::string strCharAt(const std::string& s, int64_t idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) return "";
    return std::string(1, s[static_cast<size_t>(idx)]);
}

std::string strSub(const std::string& s, int64_t start, int64_t len) {
    if (start < 0 || len <= 0 || static_cast<size_t>(start) >= s.size()) return "";
    return s.substr(static_cast<size_t>(start), static_cast<size_t>(len));
}

// ---------------------------------------------------------------------------
// Hardened process execution (no shell, mandatory timeout & maxOutput cap)
// ---------------------------------------------------------------------------
ExecResult sysExec(const std::string& command,
                   const std::vector<std::string>& args,
                   int timeoutMs,
                   size_t maxOutputBytes) {
    ExecResult res;
    if (command.empty()) return res;
    if (timeoutMs <= 0) timeoutMs = 5000;
    if (maxOutputBytes == 0) maxOutputBytes = 1024 * 1024;

#ifdef _WIN32
    auto quoteWinArg = [](const std::string& a) -> std::string {
        if (a.empty()) return "\"\"";
        if (a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
        std::string out = "\"";
        for (char c : a) {
            if (c == '\"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    };
    std::string cmdLine = quoteWinArg(command);
    for (const auto& a : args) { cmdLine.push_back(' '); cmdLine += quoteWinArg(a); }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (outR) CloseHandle(outR);
        if (outW) CloseHandle(outW);
        if (errR) CloseHandle(errR);
        if (errW) CloseHandle(errW);
        return res;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        return res;
    }

    DWORD waitRes = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutMs));
    if (waitRes == WAIT_TIMEOUT) {
        res.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
    }

    auto drainPipe = [&](HANDLE hPipe, std::string& dst) {
        char buf[4096];
        DWORD avail = 0, readBytes = 0;
        while (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (!ReadFile(hPipe, buf, sizeof(buf), &readBytes, nullptr) || readBytes == 0) break;
            if (dst.size() < maxOutputBytes) {
                size_t take = std::min(static_cast<size_t>(readBytes), maxOutputBytes - dst.size());
                dst.append(buf, take);
            }
        }
    };
    drainPipe(outR, res.stdoutStr);
    drainPipe(errR, res.stderrStr);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    res.exitCode = res.timedOut ? -1 : static_cast<int>(exitCode);

    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
#else
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        if (outPipe[0] >= 0) { close(outPipe[0]); close(outPipe[1]); }
        if (errPipe[0] >= 0) { close(errPipe[0]); close(errPipe[1]); }
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return res;
    }

    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(command.c_str(), argv.data());
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool outOpen = true, errOpen = true;
    char buf[4096];

    while (outOpen || errOpen) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.timedOut = true;
            kill(pid, SIGKILL);
            break;
        }
        int remainingMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd fds[2];
        int nfds = 0;
        int outIdx = -1, errIdx = -1;
        if (outOpen) { outIdx = nfds; fds[nfds++] = {outPipe[0], POLLIN, 0}; }
        if (errOpen) { errIdx = nfds; fds[nfds++] = {errPipe[0], POLLIN, 0}; }

        int pr = poll(fds, nfds, std::min(remainingMs, 50));
        if (pr > 0) {
            if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(outPipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (res.stdoutStr.size() < maxOutputBytes) {
                        size_t take = std::min(static_cast<size_t>(n), maxOutputBytes - res.stdoutStr.size());
                        res.stdoutStr.append(buf, take);
                    }
                } else {
                    outOpen = false;
                }
            }
            if (errIdx >= 0 && (fds[errIdx].revents & (POLLIN | POLLHUP))) {
                ssize_t n = read(errPipe[0], buf, sizeof(buf));
                if (n > 0) {
                    if (res.stderrStr.size() < maxOutputBytes) {
                        size_t take = std::min(static_cast<size_t>(n), maxOutputBytes - res.stderrStr.size());
                        res.stderrStr.append(buf, take);
                    }
                } else {
                    errOpen = false;
                }
            }
        }
    }

    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    } else {
        res.exitCode = -1;
    }
    return res;
#endif
}

// ---------------------------------------------------------------------------
// Self-contained SHA-256 (for transom.lock integrity hashes)
// ---------------------------------------------------------------------------
std::string sha256Hex(const std::string& data) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    size_t padded = ((data.size() + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    if (!data.empty()) std::memcpy(msg.data(), data.data(), data.size());
    msg[data.size()] = 0x80;
    for (int i = 0; i < 8; ++i) msg[padded - 1 - i] = static_cast<uint8_t>(bitlen >> (i * 8));

    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i*4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i*4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i*4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i*4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
        uint32_t e=state[4], f=state[5], g=state[6], h=state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }

    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int s = 28; s >= 0; s -= 4) {
            hex.push_back(kHex[(state[i] >> s) & 0xF]);
        }
    }
    return hex;
}

bool satisfiesSemver(const std::string& version, const std::string& constraint) {
    Semver v = parseSemver(version);
    if (!v.valid) return false;
    if (constraint.empty() || constraint == "*") return true;
    if (constraint.rfind(">=", 0) == 0) {
        Semver req = parseSemver(constraint.substr(2));
        return req.valid && compareSemver(v, req) >= 0;
    }
    if (constraint[0] == '^') {
        Semver req = parseSemver(constraint.substr(1));
        return req.valid && v.major == req.major && compareSemver(v, req) >= 0;
    }
    Semver req = parseSemver(constraint);
    return req.valid && compareSemver(v, req) == 0;
}

} // namespace transom::stdlib
