#pragma once

#include "common/source.hpp"
#include "frontend/ast.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transom {

struct LockedPluginEntry {
    std::string name;
    std::string version;
    std::string sha256;
    std::string path;
};

struct LoadedModuleResult {
    Program program;
    std::vector<std::string> watchedFiles;
    std::vector<std::string> styleFiles;
    std::unordered_map<std::string, std::string> fileSources;
    std::vector<LockedPluginEntry> lockedPlugins;
};

// Reads "<section> -> sha256" pairs from a transom.lock (empty map if missing).
std::unordered_map<std::string, std::string> readLockedHashes(const std::string& lockPath);

constexpr const char* kTransomVersion = "3.0.0";

// Recursively loads a .transom entry file, all `import "..."` modules, and all
// `.tplug` plugin packages referenced by `use "..."` (including transitive
// `requires.plugins` dependencies and SemVer validation).
std::optional<LoadedModuleResult> loadProgramWithImports(const std::string& entryPath,
                                                         DiagnosticEngine& diag,
                                                         const std::string* entrySourceOverride = nullptr);

bool writeLockfile(const std::string& lockPath,
                   const std::vector<LockedPluginEntry>& entries);

bool verifyLockfile(const std::string& lockPath,
                    const std::vector<LockedPluginEntry>& entries,
                    DiagnosticEngine& diag);

} // namespace transom
