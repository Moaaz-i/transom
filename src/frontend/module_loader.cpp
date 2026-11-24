#include "frontend/module_loader.hpp"

#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "stdlib/system_ops.hpp"
#include "style/style_checker.hpp"
#include "style/style_parser.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace transom {

namespace {

std::optional<std::string> readTextFile(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

fs::path findTplugPath(const std::string& nameOrPath, const fs::path& baseDir) {
    fs::path p(nameOrPath);
    std::vector<fs::path> candidates;
    if (p.extension() == ".tplug" || nameOrPath.find('/') != std::string::npos) {
        if (p.is_absolute()) candidates.push_back(p);
        else {
            if (!baseDir.empty()) candidates.push_back(baseDir / p);
            candidates.push_back(p);
        }
    } else {
        if (!baseDir.empty()) {
            fs::path cur = baseDir;
            for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
                candidates.push_back(cur / "plugins" / nameOrPath / "plugin.tplug");
                candidates.push_back(cur / "plugins" / (nameOrPath + ".tplug"));
                if (cur == cur.parent_path()) break;
                cur = cur.parent_path();
            }
        }
        candidates.push_back(fs::path("plugins") / nameOrPath / "plugin.tplug");
        candidates.push_back(fs::path("plugins") / (nameOrPath + ".tplug"));
        if (const char* home = std::getenv("HOME")) {
            candidates.push_back(fs::path(home) / ".transom" / "plugins" / nameOrPath / "plugin.tplug");
        }
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

struct LoaderState {
    DiagnosticEngine& diag;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> loaded;
    std::unordered_map<std::string, std::string> loadedTplugVersions;
    Program merged;
    std::vector<std::string> watchedFiles;
    std::vector<std::string> styleFiles;
    std::unordered_map<std::string, std::string> fileSources;
    std::vector<LockedPluginEntry> lockedPlugins;
    StyleSheetAST mergedStyles;
    const std::string* entrySourceOverride = nullptr;

    bool loadTplug(const std::string& nameOrPath, const fs::path& baseDir, Span useSpan) {
        fs::path resolved = findTplugPath(nameOrPath, baseDir);
        if (resolved.empty()) return false; // Caller may fall back to native .dylib

        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(resolved, ec);
        std::string key = ec ? resolved.string() : canonical.string();

        if (visiting.count(key)) {
            diag.error(useSpan, "Circular plugin dependency detected for '" + nameOrPath + "'");
            return true;
        }
        if (loaded.count(key)) return true;

        auto src = readTextFile(resolved);
        if (!src) {
            diag.error(useSpan, "Cannot open plugin file '" + resolved.string() + "'");
            return true;
        }

        visiting.insert(key);
        watchedFiles.push_back(key);
        fileSources[key] = *src;

        diag.setCurrentFile(resolved.string());
        Lexer lexer(*src, diag);
        auto tokens = lexer.tokenize();
        if (diag.hasErrors()) { visiting.erase(key); return true; }

        Parser parser(std::move(tokens), diag);
        Program unit = parser.parseProgram();
        if (diag.hasErrors()) { visiting.erase(key); return true; }

        if (unit.app.has_value()) {
            diag.error(unit.app->span, "Plugin file '" + resolved.string() + "' cannot contain an 'app' block");
            visiting.erase(key);
            return true;
        }
        if (unit.tplugs.size() != 1) {
            diag.error(useSpan, "Plugin file '" + resolved.string() +
                                    "' must contain exactly one 'plugin \"name\" { ... }' declaration");
            visiting.erase(key);
            return true;
        }

        PluginDecl& pl = unit.tplugs[0];
        pl.sourceHash = stdlib::sha256Hex(*src);

        // Validate Transom engine version constraint
        if (!pl.requiresTransom.empty() &&
            !stdlib::satisfiesSemver(kTransomVersion, pl.requiresTransom)) {
            diag.error(pl.span,
                       "Plugin '" + pl.name + "' requires Transom " + pl.requiresTransom +
                           ", but current runtime is " + kTransomVersion);
            visiting.erase(key);
            return true;
        }

        // Recursively load transitive plugin dependencies & validate SemVer
        for (const auto& [depName, depConstraint] : pl.requiresPlugins) {
            if (!loadTplug(depName, resolved.parent_path(), pl.span)) {
                diag.error(pl.span, "Plugin '" + pl.name + "' requires missing plugin '" + depName + "'");
                visiting.erase(key);
                return true;
            }
            auto vit = loadedTplugVersions.find(depName);
            if (vit != loadedTplugVersions.end() &&
                !stdlib::satisfiesSemver(vit->second, depConstraint)) {
                diag.error(pl.span,
                           "Plugin '" + pl.name + "' requires '" + depName + "' " + depConstraint +
                               ", found " + vit->second);
                visiting.erase(key);
                return true;
            }
        }

        loadedTplugVersions[pl.name] = pl.version;
        lockedPlugins.push_back({pl.name, pl.version, pl.sourceHash, resolved.string()});
        merged.tplugs.push_back(std::move(pl));

        visiting.erase(key);
        loaded.insert(key);
        return true;
    }

    bool loadFile(const fs::path& filePath, bool isEntry, Span importSpan) {
        std::error_code ec;
        fs::path canonical = fs::weakly_canonical(filePath, ec);
        std::string key = ec ? filePath.string() : canonical.string();

        if (visiting.count(key)) {
            diag.error(importSpan, "Circular import detected for '" + filePath.string() + "'");
            return false;
        }
        if (loaded.count(key)) return true;

        std::optional<std::string> src;
        if (isEntry && entrySourceOverride) {
            src = *entrySourceOverride;
        } else {
            src = readTextFile(filePath);
        }
        if (!src) {
            diag.error(importSpan, "Cannot open module '" + filePath.string() + "'");
            return false;
        }

        visiting.insert(key);
        watchedFiles.push_back(key);
        fileSources[key] = *src;

        diag.setCurrentFile(filePath.string());
        Lexer lexer(*src, diag);
        auto tokens = lexer.tokenize();

        Parser parser(std::move(tokens), diag);
        Program unit = parser.parseProgram();

        fs::path parentDir = filePath.parent_path();
        for (const auto& imp : unit.imports) {
            fs::path target = parentDir / imp.path;
            diag.setCurrentFile(filePath.string());
            if (!loadFile(target, false, imp.span)) {
                visiting.erase(key);
                return false;
            }
        }

        // Load .tstyle stylesheets
        for (const auto& si : unit.styleImports) {
            fs::path target = parentDir / si.path;
            std::error_code sec;
            fs::path canonStyle = fs::weakly_canonical(target, sec);
            std::string skey = sec ? target.string() : canonStyle.string();
            if (loaded.count(skey)) continue;
            auto ssrc = readTextFile(target);
            if (!ssrc) {
                diag.setCurrentFile(filePath.string());
                diag.error(si.span, "Cannot open stylesheet '" + target.string() + "'");
                visiting.erase(key);
                return false;
            }
            loaded.insert(skey);
            watchedFiles.push_back(skey);
            styleFiles.push_back(skey);
            fileSources[skey] = *ssrc;

            diag.setCurrentFile(target.string());
            StyleParser sp(*ssrc, diag);
            StyleSheetAST ast = sp.parse();
            if (diag.hasErrors()) { visiting.erase(key); return false; }
            for (auto& t : ast.tokens) mergedStyles.tokens.push_back(std::move(t));
            for (auto& th : ast.themes) mergedStyles.themes.push_back(std::move(th));
            for (auto& r : ast.rules) mergedStyles.rules.push_back(std::move(r));
        }

        // Resolve `use "..."`: prefer `.tplug` packages; keep unresolved for native `.dylib`
        for (const auto& u : unit.uses) {
            diag.setCurrentFile(filePath.string());
            if (loadTplug(u.plugin, parentDir, u.span)) {
                if (diag.hasErrors()) { visiting.erase(key); return false; }
            } else {
                merged.uses.push_back(u);
            }
        }

        for (auto& pl : unit.tplugs) {
            pl.sourceHash = stdlib::sha256Hex(*src);
            loadedTplugVersions[pl.name] = pl.version;
            lockedPlugins.push_back({pl.name, pl.version, pl.sourceHash, filePath.string()});
            merged.tplugs.push_back(std::move(pl));
        }
        for (auto& c : unit.consts) merged.consts.push_back(std::move(c));
        for (auto& s : unit.structs) merged.structs.push_back(std::move(s));
        for (auto& en : unit.enums) merged.enums.push_back(std::move(en));
        for (auto& f : unit.functions) merged.functions.push_back(std::move(f));
        for (auto& c : unit.components) merged.components.push_back(std::move(c));

        if (unit.app) {
            if (isEntry) {
                merged.app = std::move(unit.app);
            } else {
                diag.setCurrentFile(filePath.string());
                diag.warning(unit.app->span,
                             "Imported module '" + filePath.string() +
                                 "' contains an 'app' block which will be ignored");
            }
        }

        visiting.erase(key);
        loaded.insert(key);
        return true;
    }
};

} // namespace

std::optional<LoadedModuleResult> loadProgramWithImports(const std::string& entryPath,
                                                         DiagnosticEngine& diag,
                                                         const std::string* entrySourceOverride) {
    LoaderState state{diag, {}, {}, {}, {}, {}, {}, {}, {}, {}, entrySourceOverride};
    if (!state.loadFile(fs::path(entryPath), true, Span{})) {
        return std::nullopt;
    }
    if (!state.styleFiles.empty()) {
        StyleChecker sc(diag);
        state.merged.styleSheet = sc.checkAndCompile(state.mergedStyles);
        if (diag.hasErrors()) return std::nullopt;
    }
    LoadedModuleResult res;
    res.program = std::move(state.merged);
    res.watchedFiles = std::move(state.watchedFiles);
    res.styleFiles = std::move(state.styleFiles);
    res.fileSources = std::move(state.fileSources);
    res.lockedPlugins = std::move(state.lockedPlugins);
    return res;
}

bool writeLockfile(const std::string& lockPath,
                   const std::vector<LockedPluginEntry>& entries) {
    std::ofstream ofs(lockPath, std::ios::trunc);
    if (!ofs) return false;
    ofs << "# Transom 2.0 Plugin Lockfile — DO NOT EDIT MANUALLY\n";
    ofs << "transom = \"" << kTransomVersion << "\"\n\n";
    for (const auto& e : entries) {
        ofs << "[plugin." << e.name << "]\n"
            << "version = \"" << e.version << "\"\n"
            << "sha256 = \"" << e.sha256 << "\"\n"
            << "path = \"" << e.path << "\"\n\n";
    }
    return static_cast<bool>(ofs);
}

std::unordered_map<std::string, std::string> readLockedHashes(const std::string& lockPath) {
    std::unordered_map<std::string, std::string> lockedHashes;
    std::ifstream ifs(lockPath);
    if (!ifs) return lockedHashes;
    std::string currentPlugin;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = stdlib::strTrim(line);
        if (t.rfind("[plugin.", 0) == 0 && t.back() == ']') {
            currentPlugin = t.substr(8, t.size() - 9);
        } else if (!currentPlugin.empty() && t.rfind("sha256 = \"", 0) == 0 && t.back() == '"') {
            lockedHashes[currentPlugin] = t.substr(10, t.size() - 11);
        }
    }
    return lockedHashes;
}

bool verifyLockfile(const std::string& lockPath,
                    const std::vector<LockedPluginEntry>& entries,
                    DiagnosticEngine& diag) {
    if (!std::ifstream(lockPath)) return true; // No lockfile yet -> nothing to fail
    auto lockedHashes = readLockedHashes(lockPath);

    bool ok = true;
    for (const auto& e : entries) {
        auto it = lockedHashes.find(e.name);
        if (it != lockedHashes.end() && it->second != e.sha256) {
            diag.error({}, "Lockfile integrity mismatch for plugin '" + e.name +
                               "': expected sha256 " + it->second.substr(0, 12) +
                               "..., got " + e.sha256.substr(0, 12) +
                               "... (run 'transom plugin lock' to update)");
            ok = false;
        }
    }
    return ok;
}

} // namespace transom
