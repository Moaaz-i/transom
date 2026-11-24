#include "common/source.hpp"
#include "frontend/module_loader.hpp"
#include "platform/backend.hpp"
#include "plugin/plugin_manager.hpp"
#include "sema/type_checker.hpp"
#include "sema/widget_schema.hpp"
#include "style/style_checker.hpp"
#include "style/style_parser.hpp"
#include "ui/app_runtime.hpp"
#include "vm/compiler.hpp"
#include "vm/serializer.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool g_trustNative = false; // --trust-native or TRANSOM_TRUST_NATIVE=1 (development only)
bool g_useStdin = false;    // --stdin: read entry file content from stdin for live IDE checks

bool bindNativeOrReport(transom::PluginManager& plugins, transom::DiagnosticEngine& diag) {
    if (plugins.bindRuntime(diag)) return true;
    std::cerr << diag.formatAll();
    return false;
}

struct CompileOutcome {
    transom::CompiledProgram program;
    std::vector<std::string> watchedFiles;
    std::vector<std::string> styleFiles;
    std::unordered_map<std::string, std::string> fileSources;
};

std::optional<CompileOutcome> compileProject(const std::string& path,
                                             transom::PluginManager& plugins,
                                             transom::DiagnosticEngine& diag,
                                             const std::string* sourceOverride = nullptr) {
    auto loaded = transom::loadProgramWithImports(path, diag, sourceOverride);
    if (!loaded) {
        if (diag.hasErrors()) {
            std::cerr << diag.formatAll(path, sourceOverride ? *sourceOverride : "");
            diag.clear();
        }
        return std::nullopt;
    }

    auto flushDiags = [&]() {
        for (const auto& d : diag.diagnostics()) {
            std::string key = d.file.empty() ? path : d.file;
            std::error_code ec;
            auto canon = fs::weakly_canonical(key, ec);
            std::string src;
            auto it = loaded->fileSources.find(ec ? key : canon.string());
            if (it != loaded->fileSources.end()) src = it->second;
            else if (sourceOverride && key == path) src = *sourceOverride;
            std::cerr << d.format(path, src) << "\n";
        }
        diag.clear();
    };

    std::string baseDir = fs::path(path).parent_path().string();
    diag.setCurrentFile(path);
    for (const auto& u : loaded->program.uses) {
        plugins.loadPlugin(u.plugin, baseDir, u.span, diag);
    }

    // Native plugin libraries are pinned in transom.lock as "native:<name>".
    for (const auto& n : plugins.nativeLockEntries()) {
        loaded->lockedPlugins.push_back({"native:" + n.name, n.version, n.sha256, n.path});
    }

    // Verify transom.lock integrity if present next to entry file or in CWD (skip during live --stdin edit of a .tplug)
    fs::path lockPath = fs::path(path).parent_path() / "transom.lock";
    if (!fs::exists(lockPath)) lockPath = "transom.lock";
    if (!sourceOverride && !transom::verifyLockfile(lockPath.string(), loaded->lockedPlugins, diag)) {
        flushDiags();
        return std::nullopt;
    }
    plugins.setTrustPolicy(transom::readLockedHashes(lockPath.string()), g_trustNative);

    // Run TypeChecker even if Parser emitted recoverable syntax errors so the developer
    // sees all syntax + type errors together in a single check pass.
    transom::TypeChecker checker(diag, &plugins);
    checker.check(loaded->program);
    if (diag.hasErrors()) {
        flushDiags();
        return std::nullopt;
    }

    transom::Compiler compiler;
    compiler.setWidgetRegistry(&plugins.widgets());
    CompileOutcome out;
    out.program = compiler.compile(loaded->program);
    out.watchedFiles = std::move(loaded->watchedFiles);
    out.styleFiles = std::move(loaded->styleFiles);
    out.fileSources = std::move(loaded->fileSources);
    return out;
}

std::string sanitizeBundleName(const std::string& title) {
    std::string out;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
    }
    return out.empty() ? "TransomApp" : out;
}

int buildMacAppBundle(const std::string& selfExe,
                      const std::string& sourcePath,
                      const std::string& explicitOut) {
    transom::DiagnosticEngine diag;
    transom::PluginManager plugins;
    auto compiled = compileProject(sourcePath, plugins, diag);
    if (!compiled) {
        if (diag.hasErrors()) std::cerr << diag.formatAll(sourcePath);
        return 1;
    }
    if (!compiled->program.hasApp) {
        std::cerr << "transom build: '" << sourcePath << "' has no 'app' declaration.\n";
        return 1;
    }

    std::string execName = sanitizeBundleName(compiled->program.app.title);
    fs::path bundleDir = explicitOut.empty() ? fs::path(execName + ".app") : fs::path(explicitOut);
    fs::path macosDir = bundleDir / "Contents" / "MacOS";
    fs::path resDir = bundleDir / "Contents" / "Resources";

    std::error_code ec;
    fs::create_directories(macosDir, ec);
    fs::create_directories(resDir, ec);
    if (ec) {
        std::cerr << "transom build: failed to create bundle directories: " << ec.message() << "\n";
        return 1;
    }

    // 1. Save serialized bytecode into Resources/app.tbc
    fs::path tbcPath = resDir / "app.tbc";
    if (!transom::saveBytecodeFile(compiled->program, tbcPath.string())) {
        std::cerr << "transom build: failed to write " << tbcPath << "\n";
        return 1;
    }

    // 1b. Native plugins: ship manifest+library and pin their hashes at build time.
    {
        std::vector<transom::LockedPluginEntry> nativeLocks;
        for (const auto& n : plugins.nativeLockEntries()) {
            nativeLocks.push_back({"native:" + n.name, n.version, n.sha256, n.path});
        }
        for (const auto& [dirName, dir] : plugins.nativePluginDirs()) {
            fs::create_directories(resDir / "plugins", ec);
            fs::copy(dir, resDir / "plugins" / dirName,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::cerr << "transom build: failed to bundle native plugin '" << dirName << "': "
                          << ec.message() << "\n";
                return 1;
            }
        }
        if (!nativeLocks.empty() &&
            !transom::writeLockfile((resDir / "transom.lock").string(), nativeLocks)) {
            std::cerr << "transom build: failed to write bundled transom.lock\n";
            return 1;
        }
    }

    // 2. Copy native runtime binary into Contents/MacOS/<execName>
    fs::path destExe = macosDir / execName;
    fs::copy_file(selfExe, destExe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "transom build: failed to copy runtime binary: " << ec.message() << "\n";
        return 1;
    }
    fs::permissions(destExe,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);

    // 3. Generate Info.plist
    std::ofstream plist(bundleDir / "Contents" / "Info.plist", std::ios::trunc);
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n<dict>\n"
          << "  <key>CFBundleExecutable</key><string>" << execName << "</string>\n"
          << "  <key>CFBundleIdentifier</key><string>dev.transom." << execName << "</string>\n"
          << "  <key>CFBundleName</key><string>" << compiled->program.app.title << "</string>\n"
          << "  <key>CFBundlePackageType</key><string>APPL</string>\n"
          << "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
          << "  <key>NSHighResolutionCapable</key><true/>\n"
          << "</dict>\n</plist>\n";

    std::cout << "Built standalone macOS app bundle: " << bundleDir.string() << "\n";
    return 0;
}

int buildWindowsExe(const std::string& selfExe,
                    const std::string& sourcePath,
                    const std::string& explicitOut) {
    transom::DiagnosticEngine diag;
    transom::PluginManager plugins;
    auto compiled = compileProject(sourcePath, plugins, diag);
    if (!compiled) {
        if (diag.hasErrors()) std::cerr << diag.formatAll(sourcePath);
        return 1;
    }
    if (!compiled->program.hasApp) {
        std::cerr << "transom build: '" << sourcePath << "' has no 'app' declaration.\n";
        return 1;
    }

    std::string execName = sanitizeBundleName(compiled->program.app.title);
    fs::path outExe = explicitOut.empty() ? fs::path(execName + ".exe") : fs::path(explicitOut);
    fs::path outDir = outExe.has_parent_path() ? outExe.parent_path() : fs::path(".");
    std::error_code ec;
    fs::create_directories(outDir, ec);

    fs::path stubExe;
#ifdef _WIN32
    stubExe = fs::path(selfExe);
#else
    if (const char* env = std::getenv("TRANSOM_WIN_STUB")) {
        if (fs::exists(env, ec)) stubExe = env;
    }
    if (stubExe.empty() && fs::exists("build/win/transom.exe", ec)) stubExe = "build/win/transom.exe";
    if (stubExe.empty()) {
        fs::path beside = fs::path(selfExe).parent_path() / "transom-win.exe";
        if (fs::exists(beside, ec)) stubExe = beside;
    }
#endif
    if (stubExe.empty()) {
        std::cerr << "transom build --target windows: Windows stub binary not found (run 'make cross-windows' or set TRANSOM_WIN_STUB).\n";
        return 1;
    }

    fs::path tmpTbc = outDir / (execName + ".tmp.tbc");
    if (!transom::saveBytecodeFile(compiled->program, tmpTbc.string())) {
        std::cerr << "transom build: failed to serialize bytecode\n";
        return 1;
    }

    fs::copy_file(stubExe, outExe, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmpTbc, ec);
        std::cerr << "transom build: failed to copy Windows runtime stub: " << ec.message() << "\n";
        return 1;
    }

    {
        std::ifstream tbcIn(tmpTbc, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(tbcIn)), std::istreambuf_iterator<char>());
        uint64_t sz = static_cast<uint64_t>(bytes.size());
        std::ofstream exeOut(outExe, std::ios::binary | std::ios::app);
        exeOut.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        exeOut.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        exeOut.write("TBCWIN01", 8);
    }
    fs::remove(tmpTbc, ec);

    std::vector<transom::LockedPluginEntry> nativeLocks;
    for (const auto& n : plugins.nativeLockEntries()) {
        nativeLocks.push_back({"native:" + n.name, n.version, n.sha256, n.path});
    }
    for (const auto& [dirName, dir] : plugins.nativePluginDirs()) {
        fs::create_directories(outDir / "plugins", ec);
        fs::copy(dir, outDir / "plugins" / dirName,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    if (!nativeLocks.empty()) {
        transom::writeLockfile((outDir / "transom.lock").string(), nativeLocks);
    }

    std::cout << "Built standalone Windows executable: " << outExe.string() << "\n";
    return 0;
}

std::optional<transom::CompiledProgram> tryLoadEmbeddedWindowsTbc(const std::string& exePath) {
    std::ifstream ifs(exePath, std::ios::binary | std::ios::ate);
    if (!ifs) return std::nullopt;
    auto total = ifs.tellg();
    if (total < 16) return std::nullopt;
    ifs.seekg(total - static_cast<std::streamoff>(16));
    uint64_t sz = 0;
    char magic[8]{};
    ifs.read(reinterpret_cast<char*>(&sz), sizeof(sz));
    ifs.read(magic, 8);
    if (std::string(magic, 8) != "TBCWIN01") return std::nullopt;
    if (sz == 0 || static_cast<std::streamoff>(sz + 16) > total) return std::nullopt;
    ifs.seekg(total - static_cast<std::streamoff>(16 + sz));
    std::string bytes(sz, '\0');
    ifs.read(bytes.data(), static_cast<std::streamsize>(sz));
    fs::path tmp = fs::temp_directory_path() / "transom_embedded_app.tbc";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    auto prog = transom::loadBytecodeFile(tmp.string());
    std::error_code ec;
    fs::remove(tmp, ec);
    return prog;
}

int runFormatFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) { std::cerr << "transom fmt: cannot open '" << path << "'\n"; return 1; }
    std::vector<std::string> lines;
    std::string line;
    int indent = 0;
    while (std::getline(ifs, line)) {
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) {
            if (!lines.empty() && !lines.back().empty()) lines.emplace_back("");
            continue;
        }
        size_t b = line.find_last_not_of(" \t\r");
        std::string trimmed = line.substr(a, b - a + 1);
        if (!trimmed.empty() && trimmed.front() == '}') indent = std::max(0, indent - 1);
        lines.push_back(std::string(static_cast<size_t>(indent) * 4, ' ') + trimmed);
        if (!trimmed.empty() && trimmed.back() == '{') indent++;
    }
    std::ofstream ofs(path, std::ios::trunc);
    for (const auto& l : lines) ofs << l << "\n";
    std::cout << "Formatted " << path << "\n";
    return 0;
}

int runMigrateCheck(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) { std::cerr << "transom migrate: cannot open '" << path << "'\n"; return 1; }
    std::string line;
    int ln = 0;
    int findings = 0;
    std::vector<std::string> reports;
    while (std::getline(ifs, line)) {
        ln++;
        if (line.find("use \"pdf\"") != std::string::npos) {
            reports.push_back(path + ":" + std::to_string(ln) + ": [M3001] 'use \"pdf\"' is removed from core in 3.0; switch to 'use \"pdf-native\"' and pin via 'transom plugin lock'.");
            findings++;
        }
        if (line.find("use \"pdf-native\"") == std::string::npos && line.find("use \"pdf\"") != std::string::npos) {
            reports.push_back(path + ":" + std::to_string(ln) + ": [M3002] Domain widgets require an explicit 'use' plugin declaration in Transom 3.0.");
            findings++;
        }
        if (line.find("width: 1240") != std::string::npos) {
            reports.push_back(path + ":" + std::to_string(ln) + ": [M3003] Hard-coded 'width: 1240' detected; consider using 'grow: 1' or 'Grid(columns: ...)' from Transom 3.0 Layout Engine.");
            findings++;
        }
    }
    if (findings == 0) {
        std::cout << "OK: '" << path << "' is clean for Transom 3.0.\n";
        return 0;
    }
    for (const auto& r : reports) std::cout << r << "\n";
    std::cout << "Found " << findings << " migration item(s). (Read-only check; file was not modified.)\n";
    return 2;
}

int runDocGen(const std::string& outPath) {
    const auto& reg = transom::WidgetSchemaRegistry::builtins().all();
    std::vector<std::string> names;
    names.reserve(reg.size());
    for (const auto& [k, _] : reg) names.push_back(k);
    std::sort(names.begin(), names.end());

    std::ostringstream doc;
    doc << "# Transom 3.0 — UI Widgets Reference (Generated)\n\n"
        << "> Generated directly from `WidgetSchemaRegistry::builtins()` via `transom doc`.\n\n";
    for (const auto& name : names) {
        const auto& spec = reg.at(name);
        doc << "## `" << name << "`\n\n"
            << "- **Children:** " << (spec.allowsChildren ? "Yes (`{ ... }`)" : "No") << "\n"
            << "- **Click Action:** " << (spec.allowsAction ? "Yes (`{ ... }`)" : "No") << "\n"
            << "- **Positional Prop:** " << (spec.positionalProp.empty() ? "_none_" : ("`" + spec.positionalProp + "`")) << "\n"
            << "- **Platforms:** macOS (AppKit), Windows (Win32 + Direct2D)\n"
            << "- **API Version:** 3.0\n\n"
            << "| Property | Type | State Binding |\n|---|---|---|\n";
        std::vector<std::string> pnames;
        for (const auto& [pk, _] : spec.props) pnames.push_back(pk);
        std::sort(pnames.begin(), pnames.end());
        for (const auto& pn : pnames) {
            const auto& ps = spec.props.at(pn);
            std::string tn = "Any";
            switch (ps.type) {
                case transom::TypeKind::Int: tn = "Int"; break;
                case transom::TypeKind::Float: tn = "Float"; break;
                case transom::TypeKind::Bool: tn = "Bool"; break;
                case transom::TypeKind::String: tn = "String"; break;
                case transom::TypeKind::List: tn = "List"; break;
                default: break;
            }
            doc << "| `" << pn << "` | `" << tn << "` | " << (ps.requiresStateIdent ? "Required (`state`)" : "Expression") << " |\n";
        }
        doc << "\n";
    }
    if (outPath == "-") {
        std::cout << doc.str();
        return 0;
    }
    if (fs::path(outPath).has_parent_path()) fs::create_directories(fs::path(outPath).parent_path());
    std::ofstream ofs(outPath, std::ios::trunc);
    ofs << doc.str();
    std::cout << "Generated widget reference (" << names.size() << " widgets) -> " << outPath << "\n";
    return 0;
}

int runLspServer() {
    auto writeMsg = [](const std::string& json) {
        std::cout << "Content-Length: " << json.size() << "\r\n\r\n" << json << std::flush;
    };
    const auto& reg = transom::WidgetSchemaRegistry::builtins().all();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.rfind("Content-Length:", 0) != 0) continue;
        size_t len = static_cast<size_t>(std::stoul(line.substr(15)));
        while (std::getline(std::cin, line) && line != "\r" && !line.empty()) {}
        std::string body(len, '\0');
        std::cin.read(body.data(), static_cast<std::streamsize>(len));
        if (body.find("\"method\":\"shutdown\"") != std::string::npos) {
            writeMsg("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
            break;
        }
        if (body.find("\"method\":\"initialize\"") != std::string::npos) {
            writeMsg("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"completionProvider\":{\"triggerCharacters\":[\".\"]},\"hoverProvider\":true}}}");
        } else if (body.find("\"method\":\"textDocument/completion\"") != std::string::npos) {
            std::ostringstream items;
            items << "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":[";
            bool first = true;
            for (const auto& [wn, _] : reg) {
                if (!first) items << ",";
                first = false;
                items << "{\"label\":\"" << wn << "\",\"kind\":7,\"detail\":\"Transom 3.0 Widget\"}";
            }
            items << "]}";
            writeMsg(items.str());
        } else if (body.find("\"method\":\"textDocument/hover\"") != std::string::npos) {
            writeMsg("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"contents\":\"Transom 3.0 Language Server\"}}");
        }
    }
    return 0;
}

int scaffoldNewProject(const std::string& name) {
    fs::path dir(name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "transom new: failed to create '" << name << "': " << ec.message() << "\n";
        return 1;
    }

    std::ofstream(dir / "components.transom") << R"(component Header(title: String, subtitle: String) {
    VStack(gap: 4) {
        Text(title, size: 22, weight: "bold", color: "#f8fafc")
        Text(subtitle, size: 13, color: "#94a3b8")
    }
}
)";

    std::ofstream(dir / "main.transom") << "import \"./components.transom\"\n\n"
        << "app \"" << name << "\" (width: 440, height: 320, bg: \"#14171f\") {\n"
        << "    state count = 0\n\n"
        << "    view VStack(gap: 16, padding: 24) {\n"
        << "        Header(title: \"" << name << "\", subtitle: \"Built with Transom 2.0\")\n"
        << "        Card(padding: 16, bg: \"#1e2330\", radius: 12) {\n"
        << "            Text(\"Count: {count}\", size: 24, weight: \"bold\")\n"
        << "        }\n"
        << "        HStack(gap: 10) {\n"
        << "            Button(\"-\") { count -= 1 }\n"
        << "            Button(\"+\", style: \"primary\") { count += 1 }\n"
        << "        }\n"
        << "    }\n"
        << "}\n";

    std::cout << "Created Transom project in ./" << name << "/\n"
              << "Run it with:\n"
              << "  transom watch " << name << "/main.transom\n";
    return 0;
}

int scaffoldNewPlugin(const std::string& name) {
    fs::path dir = fs::path("plugins") / name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "transom plugin new: failed to create " << dir << ": " << ec.message() << "\n";
        return 1;
    }
    fs::path file = dir / "plugin.tplug";
    std::ofstream ofs(file, std::ios::trunc);
    ofs << "plugin \"" << name << "\" {\n"
        << "    version: \"1.0.0\"\n"
        << "    description: \"" << name << " official Transom module\"\n"
        << "    requires: {\n"
        << "        transom: \">=2.0.0\"\n"
        << "    }\n"
        << "    permissions: []\n\n"
        << "    export fn hello(target: String) -> String {\n"
        << "        return \"Hello from " << name << ", {target}!\"\n"
        << "    }\n"
        << "}\n";
    std::cout << "Created Transom plugin module: " << file.string() << "\n";
    return 0;
}

void printUsage() {
    std::cout << "Transom 3.0 — Cross-Platform Declarative Reactive Native UI Language\n\n"
              << "Usage:\n"
              << "  transom run <file.transom>              Compile and launch native app\n"
              << "  transom watch <file.transom>            Launch with live Hot Reload\n"
              << "  transom build <file.transom> [--target macos|windows] [--out X]\n"
              << "  transom fmt <file.transom>              Format Transom source code\n"
              << "  transom migrate <file.transom>          Check 2.0 projects for 3.0 migration\n"
              << "  transom doc [out.md]                    Generate widget reference from registry\n"
              << "  transom lsp                             Run Language Server Protocol on stdio\n"
              << "  transom new <project-name>              Scaffold a new Transom project\n"
              << "  transom add <plugin-name-or-path>       Install a .tplug or .tnative plugin into ./plugins/\n"
              << "  transom plugin new <name>               Scaffold a new .tplug module\n"
              << "  transom plugin check <file.tplug>       Validate .tplug contract & permissions\n"
              << "  transom plugin lock <file.transom>      Generate transom.lock with SHA-256 hashes\n"
              << "  transom check <file.transom>            Parse and type-check without UI\n"
              << "  transom disasm <file.transom>           Print compiled VM bytecode\n";
}

} // namespace

int main(int argc, char* argv[]) {
    // Strip global flags so positional parsing below is unaffected.
    if (const char* env = std::getenv("TRANSOM_TRUST_NATIVE")) g_trustNative = std::string(env) == "1";
    {
        int w = 1;
        for (int r = 1; r < argc; ++r) {
            if (std::string(argv[r]) == "--trust-native") { g_trustNative = true; continue; }
            if (std::string(argv[r]) == "--stdin") { g_useStdin = true; continue; }
            argv[w++] = argv[r];
        }
        argc = w;
    }
    // Bundled .app mode: if launched without arguments and ../Resources/app.tbc exists, run it!
    if (argc < 2) {
        std::error_code ec;
        fs::path exePath = fs::weakly_canonical(argv[0], ec);
        if (auto winProg = tryLoadEmbeddedWindowsTbc(exePath.string()); winProg && winProg->hasApp) {
            transom::PluginManager plugins;
            transom::DiagnosticEngine diag;
            for (const auto& p : winProg->plugins) plugins.loadPlugin(p, exePath.parent_path().string(), {}, diag);
            plugins.setTrustPolicy(transom::readLockedHashes((exePath.parent_path() / "transom.lock").string()), false);
            if (!bindNativeOrReport(plugins, diag)) return 1;
            auto backend = transom::createNativeBackend();
            transom::AppRuntime runtime(*backend, &plugins);
            runtime.mount(std::move(*winProg));
            backend->runEventLoop();
            return 0;
        }
        fs::path bundledTbc = exePath.parent_path().parent_path() / "Resources" / "app.tbc";
        if (!ec && fs::exists(bundledTbc, ec)) {
            auto prog = transom::loadBytecodeFile(bundledTbc.string());
            if (prog && prog->hasApp) {
                transom::PluginManager plugins;
                transom::DiagnosticEngine diag;
                for (const auto& p : prog->plugins) {
                    plugins.loadPlugin(p, bundledTbc.parent_path().string(), {}, diag);
                }
                plugins.setTrustPolicy(
                    transom::readLockedHashes((bundledTbc.parent_path() / "transom.lock").string()),
                    false);
                if (!bindNativeOrReport(plugins, diag)) return 1;
                auto backend = transom::createNativeBackend();
                transom::AppRuntime runtime(*backend, &plugins);
                runtime.mount(std::move(*prog));
                backend->runEventLoop();
                return 0;
            }
        }
        printUsage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "-h" || cmd == "--help") {
        printUsage();
        return 0;
    }
    if (cmd == "add") {
        if (argc < 3) {
            std::cerr << "Usage: transom add <plugin-name-or-path>\n";
            return 1;
        }
        std::string target = argv[2];
        fs::path srcDir;
        std::error_code ec;
        if (fs::exists(target, ec) && fs::is_directory(target, ec)) {
            srcDir = fs::weakly_canonical(target, ec);
        } else {
            std::vector<fs::path> search = {
                fs::path("..") / "Transom-Plugins" / target,
                fs::path("plugins") / target,
            };
            if (const char* home = std::getenv("HOME")) {
                search.push_back(fs::path(home) / "Desktop" / "Transom-Plugins" / target);
                search.push_back(fs::path(home) / ".transom" / "plugins" / target);
            }
            for (const auto& cand : search) {
                if (fs::exists(cand, ec) && fs::is_directory(cand, ec)) {
                    srcDir = fs::weakly_canonical(cand, ec);
                    break;
                }
            }
        }
        if (srcDir.empty()) {
            std::cerr << "transom add: could not find plugin '" << target
                      << "' (pass a directory path or place it in ../Transom-Plugins/ or ~/.transom/plugins/)\n";
            return 1;
        }
        std::string pname = srcDir.filename().string();
        fs::path dstDir = fs::path("plugins") / pname;
        fs::create_directories("plugins", ec);
        fs::copy(srcDir, dstDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "transom add: failed to copy plugin: " << ec.message() << "\n";
            return 1;
        }
        // Validate official protected plugin format (.tplug or plugin.tnative)
        fs::path tplugFile = dstDir / "plugin.tplug";
        fs::path tnativeFile = dstDir / "plugin.tnative";
        if (fs::exists(tplugFile, ec)) {
            transom::DiagnosticEngine pdiag;
            transom::PluginManager pm;
            if (!compileProject(tplugFile.string(), pm, pdiag)) {
                if (pdiag.hasErrors()) std::cerr << pdiag.formatAll(tplugFile.string());
                return 1;
            }
            std::cout << "Verified protected .tplug module: " << tplugFile.string() << "\n";
        } else if (fs::exists(tnativeFile, ec)) {
            std::ifstream ifs(tnativeFile);
            std::ostringstream oss; oss << ifs.rdbuf();
            transom::DiagnosticEngine mdiag;
            mdiag.setCurrentFile(tnativeFile.string());
            transom::NativeManifest nm;
            if (!transom::parseNativeManifest(oss.str(), tnativeFile.string(), nm, mdiag) || mdiag.hasErrors()) {
                std::cerr << mdiag.formatAll(tnativeFile.string(), oss.str());
                return 1;
            }
            std::cout << "Verified protected .tnative manifest (ABI v" << nm.api << "): "
                      << tnativeFile.string() << "\n";
        } else {
            std::cerr << "transom add: rejected '" << pname
                      << "' — missing official 'plugin.tplug' or 'plugin.tnative' manifest\n";
            return 1;
        }
        std::cout << "Added plugin '" << pname << "' -> " << dstDir.string() << "\n"
                  << "Import it in your app with:\n  use \"" << pname << "\"\n"
                  << "Then pin its SHA-256 hash with:\n  transom plugin lock <your-app.transom>\n";
        return 0;
    }
    if (cmd == "new") {
        if (argc < 3) {
            std::cerr << "transom new: missing project name\n";
            return 1;
        }
        return scaffoldNewProject(argv[2]);
    }
    if (cmd == "plugin") {
        if (argc < 4) {
            std::cerr << "Usage: transom plugin <new|check|lock> <arg>\n";
            return 1;
        }
        std::string sub = argv[2];
        std::string arg = argv[3];
        if (sub == "new") return scaffoldNewPlugin(arg);
        if (sub == "check") {
            transom::DiagnosticEngine diag;
            transom::PluginManager plugins;
            auto compiled = compileProject(arg, plugins, diag);
            if (!compiled) {
                if (diag.hasErrors()) std::cerr << diag.formatAll(arg);
                return 1;
            }
            std::cout << "OK: plugin '" << arg << "' passed contract, type & capability checks.\n";
            return 0;
        }
        if (sub == "lock") {
            transom::DiagnosticEngine diag;
            auto loaded = transom::loadProgramWithImports(arg, diag);
            if (!loaded || diag.hasErrors()) {
                if (diag.hasErrors()) std::cerr << diag.formatAll(arg);
                return 1;
            }
            {
                transom::PluginManager lockPlugins;
                for (const auto& u : loaded->program.uses) {
                    lockPlugins.loadPlugin(u.plugin, fs::path(arg).parent_path().string(), u.span, diag);
                }
                if (diag.hasErrors()) {
                    std::cerr << diag.formatAll(arg);
                    return 1;
                }
                for (const auto& n : lockPlugins.nativeLockEntries()) {
                    loaded->lockedPlugins.push_back({"native:" + n.name, n.version, n.sha256, n.path});
                }
            }
            fs::path lockPath = fs::path(arg).parent_path() / "transom.lock";
            if (!transom::writeLockfile(lockPath.string(), loaded->lockedPlugins)) {
                std::cerr << "transom plugin lock: failed to write " << lockPath << "\n";
                return 1;
            }
            std::cout << "Wrote " << lockPath.string() << " (" << loaded->lockedPlugins.size()
                      << " plugin(s) locked).\n";
            return 0;
        }
        std::cerr << "Unknown plugin subcommand '" << sub << "'\n";
        return 1;
    }
    if (cmd == "fmt") {
        if (argc < 3) { std::cerr << "transom fmt: missing file\n"; return 1; }
        return runFormatFile(argv[2]);
    }
    if (cmd == "migrate") {
        if (argc < 3) { std::cerr << "transom migrate: missing file\n"; return 1; }
        return runMigrateCheck(argv[2]);
    }
    if (cmd == "doc") {
        return runDocGen(argc >= 3 ? argv[2] : "docs/04-ui-widgets.md");
    }
    if (cmd == "lsp") {
        return runLspServer();
    }
    if (cmd == "build") {
        if (argc < 3) {
            std::cerr << "transom build: missing source file\n";
            return 1;
        }
        std::string outPath;
        std::string target =
#ifdef _WIN32
            "windows";
#else
            "macos";
#endif
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--out") outPath = argv[i + 1];
            if (std::string(argv[i]) == "--target") target = argv[i + 1];
        }
        if (target == "windows") return buildWindowsExe(argv[0], argv[2], outPath);
        return buildMacAppBundle(argv[0], argv[2], outPath);
    }

    std::string path;
    if (cmd == "run" || cmd == "watch" || cmd == "check" || cmd == "disasm") {
        if (argc < 3) {
            std::cerr << "transom: missing file path for '" << cmd << "'\n";
            return 1;
        }
        path = argv[2];
    } else {
        path = cmd;
        cmd = "run";
    }

    // Direct .tbc bytecode execution
    if (cmd == "run" && path.size() > 4 && path.substr(path.size() - 4) == ".tbc") {
        auto prog = transom::loadBytecodeFile(path);
        if (!prog || !prog->hasApp) {
            std::cerr << "transom: failed to load bytecode file '" << path << "'\n";
            return 1;
        }
        transom::PluginManager plugins;
        transom::DiagnosticEngine diag;
        for (const auto& p : prog->plugins) {
            plugins.loadPlugin(p, fs::path(path).parent_path().string(), {}, diag);
        }
        plugins.setTrustPolicy(
            transom::readLockedHashes((fs::path(path).parent_path() / "transom.lock").string()),
            g_trustNative);
        if (!bindNativeOrReport(plugins, diag)) return 1;
        auto backend = transom::createNativeBackend();
        transom::AppRuntime runtime(*backend, &plugins);
        runtime.mount(std::move(*prog));
        backend->runEventLoop();
        return 0;
    }

    std::string stdinSource;
    const std::string* sourceOverride = nullptr;
    if (g_useStdin) {
        std::ostringstream sss;
        sss << std::cin.rdbuf();
        stdinSource = sss.str();
        sourceOverride = &stdinSource;
    }

    // Direct .tnative manifest check
    if (cmd == "check" && path.size() > 8 && path.substr(path.size() - 8) == ".tnative") {
        std::string src;
        if (sourceOverride) {
            src = *sourceOverride;
        } else {
            std::ifstream ifs(path);
            if (!ifs) { std::cerr << "transom: cannot open manifest '" << path << "'\n"; return 1; }
            std::ostringstream oss; oss << ifs.rdbuf(); src = oss.str();
        }
        transom::DiagnosticEngine mdiag;
        mdiag.setCurrentFile(path);
        transom::NativeManifest nm;
        if (!transom::parseNativeManifest(src, path, nm, mdiag) || mdiag.hasErrors()) {
            std::cerr << mdiag.formatAll(path, src);
            return 1;
        }
        std::cout << "OK: " << path << " passed native plugin manifest checks.\n";
        return 0;
    }

    // Direct .tstyle stylesheet check
    if (cmd == "check" && path.size() > 7 && path.substr(path.size() - 7) == ".tstyle") {
        std::string src;
        if (sourceOverride) {
            src = *sourceOverride;
        } else {
            std::ifstream ifs(path);
            if (!ifs) {
                std::cerr << "transom: cannot open stylesheet '" << path << "'\n";
                return 1;
            }
            std::ostringstream oss;
            oss << ifs.rdbuf();
            src = oss.str();
        }

        transom::DiagnosticEngine sdiag;
        sdiag.setCurrentFile(path);
        transom::StyleParser sp(src, sdiag);
        auto ast = sp.parse();
        if (!sdiag.hasErrors()) {
            transom::StyleChecker sc(sdiag);
            sc.checkAndCompile(ast);
        }
        if (sdiag.hasErrors()) {
            std::cerr << sdiag.formatAll(path, src);
            return 1;
        }
        std::cout << "OK: " << path << " passed stylesheet syntax & type checks.\n";
        return 0;
    }

    transom::DiagnosticEngine diag;
    transom::PluginManager plugins;
    auto compiled = compileProject(path, plugins, diag, sourceOverride);
    if (!compiled) {
        if (diag.hasErrors()) std::cerr << diag.formatAll(path);
        return 1;
    }

    if (cmd == "check") {
        std::cout << "OK: " << path << " passed syntax & type checks ("
                  << compiled->watchedFiles.size() << " module(s), "
                  << compiled->program.plugins.size() << " plugin(s)).\n";
        return 0;
    }

    if (cmd == "disasm") {
        std::cout << compiled->program.disassemble();
        return 0;
    }

    if (!compiled->program.hasApp) {
        std::cerr << "transom: '" << path << "' has no 'app' declaration to launch.\n";
        return 1;
    }

    if (!bindNativeOrReport(plugins, diag)) return 1;
    auto backend = transom::createNativeBackend();
    transom::AppRuntime runtime(*backend, &plugins);
    auto watchedFiles = compiled->watchedFiles;
    auto styleFiles = compiled->styleFiles;
    runtime.mount(std::move(compiled->program));

    if (cmd == "watch") {
        auto snapshotTimes = [](const std::vector<std::string>& files) {
            std::unordered_map<std::string, fs::file_time_type> m;
            for (const auto& f : files) {
                std::error_code ec;
                auto t = fs::last_write_time(f, ec);
                if (!ec) m[f] = t;
            }
            return m;
        };
        auto mtimes = snapshotTimes(watchedFiles);
        std::cout << "[transom watch] Watching " << watchedFiles.size() << " file(s) for " << path << "\n";

        backend->runEventLoop([&]() {
            bool anyChanged = false;
            bool nonStyleChanged = false;
            std::unordered_set<std::string> styleSet(styleFiles.begin(), styleFiles.end());
            for (const auto& f : watchedFiles) {
                std::error_code ec;
                auto now = fs::last_write_time(f, ec);
                if (!ec && (!mtimes.count(f) || mtimes[f] != now)) {
                    anyChanged = true;
                    if (!styleSet.count(f)) nonStyleChanged = true;
                }
            }
            if (anyChanged) {
                transom::DiagnosticEngine reloadDiag;
                auto next = compileProject(path, plugins, reloadDiag);
                if (next && next->program.hasApp) {
                    watchedFiles = next->watchedFiles;
                    styleFiles = next->styleFiles;
                    mtimes = snapshotTimes(watchedFiles);
                    if (!nonStyleChanged) {
                        std::cout << "[transom watch] Hot-reloaded styles (state preserved)\n";
                        runtime.reloadStyle(std::move(next->program.styleSheet));
                    } else {
                        std::cout << "[transom watch] Reloaded " << path << "\n";
                        runtime.reload(std::move(next->program));
                    }
                } else {
                    mtimes = snapshotTimes(watchedFiles);
                    if (reloadDiag.hasErrors()) {
                        std::cerr << "[transom watch] Reload failed:\n"
                                  << reloadDiag.formatAll(path);
                    }
                }
            }
        });
    } else {
        backend->runEventLoop();
    }

    return 0;
}
