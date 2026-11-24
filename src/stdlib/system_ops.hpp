#pragma once

#include "common/value.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace transom {

namespace stdlib {

// File I/O & App Storage
std::string fsRead(const std::string& path);
bool fsWrite(const std::string& path, const std::string& content);
bool fsExists(const std::string& path);

std::string storageGet(const std::string& appName,
                       const std::string& key,
                       const std::string& fallback);
void storageSet(const std::string& appName,
                const std::string& key,
                const std::string& value);

// JSON
std::string jsonGet(const std::string& json, const std::string& key);
std::string jsonPath(const std::string& json, const std::string& dottedPath);

// First-class Bytes operations
ValueBytes bytesRead(const std::string& path);
ValueBytes bytesSlice(const ValueBytes& b, int64_t start, int64_t len);
std::string bytesToString(const ValueBytes& b);
std::string bytesToHex(const ValueBytes& b);

// String utilities (for writing parsers & plugins in pure Transom)
std::string strTrim(const std::string& s);
std::vector<std::string> strSplit(const std::string& s, const std::string& sep);
bool strContains(const std::string& s, const std::string& sub);
bool strStartsWith(const std::string& s, const std::string& prefix);
std::string strReplace(const std::string& s, const std::string& from, const std::string& to);
std::string strCharAt(const std::string& s, int64_t idx);
std::string strSub(const std::string& s, int64_t start, int64_t len);

// Hardened process execution (no shell, mandatory timeout & output cap)
struct ExecResult {
    int64_t exitCode = -1;
    std::string stdoutStr;
    std::string stderrStr;
    bool timedOut = false;
};

ExecResult sysExec(const std::string& command,
                   const std::vector<std::string>& args,
                   int timeoutMs,
                   size_t maxOutputBytes);

// Cryptographic hash (SHA-256) for transom.lock integrity
std::string sha256Hex(const std::string& data);

// SemVer constraint checker ("2.0.0", ">=2.0.0", "^1.2.0")
bool satisfiesSemver(const std::string& version, const std::string& constraint);

} // namespace stdlib

} // namespace transom
