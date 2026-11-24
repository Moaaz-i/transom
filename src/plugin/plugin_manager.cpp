#include "plugin/plugin_manager.hpp"

#include "stdlib/system_ops.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace transom {

namespace {

#if defined(_WIN32)
constexpr const char* kPlatform = "windows";
#elif defined(__APPLE__)
constexpr const char* kPlatform = "macos";
#else
constexpr const char* kPlatform = "linux";
#endif

void* openLib(const std::string& path, std::string& err) {
#ifdef _WIN32
    HMODULE h = LoadLibraryW(fs::path(path).wstring().c_str());
    if (!h) err = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { const char* e = dlerror(); err = e ? e : "unknown dlopen error"; }
    return h;
#endif
}

void* findSym(void* h, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
    return dlsym(h, name);
#endif
}

void closeLib(void* h) {
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
    dlclose(h);
#endif
}

std::string readBinary(const fs::path& p, bool& ok) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) { ok = false; return {}; }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    ok = true;
    return ss.str();
}

std::string sanitizeNs(std::string s) {
    for (char& c : s) if (c == '-') c = '_';
    return s;
}

fs::path findManifest(const std::string& name, const std::string& baseDir) {
    std::vector<fs::path> candidates;
    auto addRoot = [&](const fs::path& root) {
        candidates.push_back(root / name / "plugin.tnative");
    };
    if (!baseDir.empty()) {
        fs::path cur = fs::absolute(fs::path(baseDir));
        for (int i = 0; i < 4; ++i) {
            addRoot(cur / "plugins");
            if (!cur.has_parent_path() || cur.parent_path() == cur) break;
            cur = cur.parent_path();
        }
    }
    addRoot(fs::path("plugins"));
    if (const char* env = std::getenv("TRANSOM_PLUGIN_PATH")) {
        std::string all = env;
        size_t start = 0;
        while (start <= all.size()) {
#ifdef _WIN32
            const char sep = ';';
#else
            const char sep = ':';
#endif
            size_t end = all.find(sep, start);
            std::string part = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) addRoot(fs::path(part));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (home) addRoot(fs::path(home) / ".transom" / "plugins");

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return {};
}

} // namespace

PluginManager::PluginManager() : widgets_(WidgetSchemaRegistry::builtins()) {}

PluginManager::~PluginManager() {
    for (auto& p : plugins_) {
        if (!p.dlHandle) continue;
        using ShutdownFn = void (*)();
        if (auto sd = reinterpret_cast<ShutdownFn>(findSym(p.dlHandle, "transom_plugin_shutdown"))) sd();
        closeLib(p.dlHandle);
    }
}

Type PluginManager::fromCAbiType(TransomType t) {
    switch (t) {
        case TRANSOM_TYPE_VOID: return Type::voidT();
        case TRANSOM_TYPE_INT: return Type::intT();
        case TRANSOM_TYPE_FLOAT: return Type::floatT();
        case TRANSOM_TYPE_BOOL: return Type::boolT();
        case TRANSOM_TYPE_STRING: return Type::stringT();
        case TRANSOM_TYPE_ANY: return Type::anyT();
    }
    return Type::anyT();
}

void PluginManager::setTrustPolicy(std::unordered_map<std::string, std::string> lockedHashes,
                                   bool trustAll) {
    lockedHashes_ = std::move(lockedHashes);
    trustAll_ = trustAll;
}

bool PluginManager::loadPlugin(const std::string& nameOrPath,
                               const std::string& baseDir,
                               Span span,
                               DiagnosticEngine& diag) {
    std::string name = fs::path(nameOrPath).stem().string();
    if (name.empty()) name = nameOrPath;
    std::string ns = sanitizeNs(name);
    if (namespaces_.count(ns)) return true; // already loaded

    fs::path manifestPath = findManifest(name, baseDir);
    if (manifestPath.empty()) {
        diag.error(span, "Plugin '" + nameOrPath + "' not found. Looked for 'plugins/" + name +
                             "/plugin.tplug' (Transom module) and 'plugins/" + name +
                             "/plugin.tnative' (native plugin). Native plugins need a plugin.tnative "
                             "manifest since Transom 3.0 (see docs/contracts/native-plugin-abi.md).");
        return false;
    }

    bool ok = false;
    std::string text = readBinary(manifestPath, ok);
    if (!ok) {
        diag.error(span, "Cannot read native plugin manifest '" + manifestPath.string() + "'");
        return false;
    }

    NativeManifest m;
    if (!parseNativeManifest(text, manifestPath.string(), m, diag)) return false;

    if (m.api < TRANSOM_PLUGIN_API_MIN_VERSION || m.api > TRANSOM_PLUGIN_API_VERSION) {
        diag.error(span, "Native plugin '" + m.name + "' targets plugin API v" + std::to_string(m.api) +
                             " but this Transom supports v" + std::to_string(TRANSOM_PLUGIN_API_MIN_VERSION) +
                             "..v" + std::to_string(TRANSOM_PLUGIN_API_VERSION));
        return false;
    }

    for (const auto& w : m.widgets) {
        if (!widgets_.add(w)) {
            diag.error(span, "Native plugin '" + m.name + "' declares widget '" + w.name +
                                 "' which already exists (built-in or from another plugin)");
            return false;
        }
    }
    for (const auto& fn : m.fns) {
        PluginFnInfo info;
        info.qualifiedName = m.ns + "." + fn.name;
        info.params = fn.params;
        info.returnType = fn.returnType;
        functions_[info.qualifiedName] = std::move(info);
    }

    Loaded l;
    auto lib = m.libraries.find(kPlatform);
    if (lib != m.libraries.end()) l.libraryPath = (fs::path(m.dir) / lib->second).string();
    l.manifest = std::move(m);
    namespaces_[ns] = true;
    plugins_.push_back(std::move(l));
    return true;
}

std::vector<NativeLockInfo> PluginManager::nativeLockEntries() const {
    std::vector<NativeLockInfo> out;
    for (const auto& p : plugins_) {
        if (p.libraryPath.empty()) continue;
        bool ok = false;
        std::string bytes = readBinary(p.libraryPath, ok);
        if (!ok) continue;
        out.push_back({p.manifest.name, p.manifest.version, stdlib::sha256Hex(bytes), p.libraryPath});
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> PluginManager::nativePluginDirs() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& p : plugins_) {
        out.emplace_back(fs::path(p.manifest.dir).filename().string(), p.manifest.dir);
    }
    return out;
}

void PluginManager::hostRegisterFn(void* hostCtx, const char* fnName, const TransomType* paramTypes,
                                   int paramCount, TransomType returnType, TransomFnCallback cb) {
    auto* self = static_cast<PluginManager*>(hostCtx);
    if (!self || !fnName || !cb) return;
    std::string qn = self->activeNamespace_ + "." + fnName;
    auto it = self->functions_.find(qn);
    if (it == self->functions_.end()) {
        self->activeError_ = "plugin registered function '" + std::string(fnName) +
                             "' which is not declared in plugin.tnative";
        return;
    }
    PluginFnInfo& info = it->second;
    bool match = static_cast<int>(info.params.size()) == paramCount &&
                 info.returnType.kind == fromCAbiType(returnType).kind;
    for (int i = 0; match && i < paramCount; ++i) {
        match = info.params[static_cast<size_t>(i)].kind == fromCAbiType(paramTypes[i]).kind;
    }
    if (!match) {
        self->activeError_ = "function '" + std::string(fnName) +
                             "' signature differs from plugin.tnative declaration";
        return;
    }
    info.callback = cb;
}

const char* PluginManager::hostAllocString(void* hostCtx, const char* utf8, size_t len) {
    auto* self = static_cast<PluginManager*>(hostCtx);
    if (!self || !utf8) return "";
    self->stringArena_.emplace_back(utf8, len);
    return self->stringArena_.back().c_str();
}

int PluginManager::hostRegisterWidget(void* hostCtx, const char* name, const TransomWidgetVTable* vt) {
    auto* self = static_cast<PluginManager*>(hostCtx);
    if (!self || !name || !vt) return 1;
    const WidgetSpec* spec = self->widgets_.find(name);
    if (!spec || spec->ownerPlugin.empty()) {
        self->activeError_ = "plugin registered widget '" + std::string(name) +
                             "' which is not declared in plugin.tnative";
        return 2;
    }
    if (vt->struct_size < sizeof(TransomWidgetVTable) || !vt->create || !vt->set_prop ||
        !vt->native_handle || !vt->destroy) {
        self->activeError_ = "widget '" + std::string(name) + "' has an incomplete vtable";
        return 3;
    }
    self->widgetVtables_[name] = *vt;
    return 0;
}

void PluginManager::fillHostApi(TransomHostAPI& api) {
    api.struct_size = sizeof(TransomHostAPI);
    api.api_version = TRANSOM_PLUGIN_API_VERSION;
    api.host_ctx = this;
    api.platform = kPlatform;
    api.register_fn = &PluginManager::hostRegisterFn;
    api.alloc_string = &PluginManager::hostAllocString;
    api.register_widget = &PluginManager::hostRegisterWidget;
    api.set_error = &PluginManager::hostSetError;
    api.emit_widget_event = &PluginManager::hostEmitWidgetEvent;
}

void PluginManager::hostEmitWidgetEvent(void* hostCtx, void* instance, const char* event,
                                        const TransomValue* value) {
    auto* self = static_cast<PluginManager*>(hostCtx);
    if (!self || !instance) return;
    auto it = self->eventSinks_.find(instance);
    if (it == self->eventSinks_.end() || !it->second) return;
    Value payload(Null{});
    if (value) {
        switch (value->kind) {
            case TRANSOM_VAL_INT: payload = Value(value->i); break;
            case TRANSOM_VAL_FLOAT: payload = Value(value->f); break;
            case TRANSOM_VAL_BOOL: payload = Value(value->b != 0); break;
            case TRANSOM_VAL_STRING: payload = Value(value->s ? std::string(value->s) : std::string()); break;
            default: break;
        }
    }
    self->lastEventName_ = event ? event : "";
    self->lastEventPayload_ = payload.isNull() ? "" : payload.toString();
    auto sink = it->second; // copy: the handler may re-enter the plugin
    sink(self->lastEventName_, payload);
}

void PluginManager::setNativeWidgetEventSink(
    const std::string&, void* instance,
    std::function<void(const std::string&, const Value&)> sink) {
    if (instance) eventSinks_[instance] = std::move(sink);
}

void PluginManager::hostSetError(void* hostCtx, const char* message) {
    auto* self = static_cast<PluginManager*>(hostCtx);
    if (self && message) self->activeError_ = message;
}

bool PluginManager::bindRuntime(DiagnosticEngine& diag) {
    for (auto& p : plugins_) {
        if (p.bound) continue;
        const std::string& name = p.manifest.name;

        if (p.libraryPath.empty()) {
            diag.error(Span{}, "Native plugin '" + name + "' has no library for platform '" +
                                   std::string(kPlatform) + "' in plugin.tnative");
            return false;
        }
        bool ok = false;
        std::string bytes = readBinary(p.libraryPath, ok);
        if (!ok) {
            diag.error(Span{}, "Native plugin library not found: " + p.libraryPath +
                                   " (build the plugin first)");
            return false;
        }

        // Trust policy: a native library runs with full privileges, so it must be pinned.
        if (!trustAll_) {
            std::string actual = stdlib::sha256Hex(bytes);
            auto it = lockedHashes_.find("native:" + name);
            if (it == lockedHashes_.end()) {
                diag.error(Span{}, "Native plugin '" + name + "' is not trusted. Pin it with "
                                       "'transom plugin lock <app.transom>' (or run with --trust-native "
                                       "during development).");
                return false;
            }
            if (it->second != actual) {
                diag.error(Span{}, "Native plugin '" + name + "' library hash mismatch (expected " +
                                       it->second.substr(0, 12) + "..., got " + actual.substr(0, 12) +
                                       "...). Refusing to load. Re-lock if the update is intended.");
                return false;
            }
        }

        std::string err;
        void* h = openLib(p.libraryPath, err);
        if (!h) {
            diag.error(Span{}, "Cannot load native plugin '" + name + "': " + err);
            return false;
        }
        using InitFn = int (*)(const TransomHostAPI*);
        auto init = reinterpret_cast<InitFn>(findSym(h, "transom_plugin_init"));
        if (!init) {
            closeLib(h);
            diag.error(Span{}, "Native plugin '" + name + "' does not export 'transom_plugin_init'");
            return false;
        }

        TransomHostAPI api{};
        fillHostApi(api);

        activeNamespace_ = p.manifest.ns;
        activeError_.clear();
        int rc = init(&api);
        activeNamespace_.clear();

        if (rc != 0 || !activeError_.empty()) {
            closeLib(h);
            diag.error(Span{}, "Native plugin '" + name + "' initialization failed" +
                                   (activeError_.empty() ? " (code " + std::to_string(rc) + ")"
                                                         : ": " + activeError_));
            return false;
        }

        // Every declared function and widget must have been bound.
        for (const auto& fn : p.manifest.fns) {
            const PluginFnInfo* info = findFunction(p.manifest.ns + "." + fn.name);
            if (!info || !info->callback) {
                closeLib(h);
                diag.error(Span{}, "Native plugin '" + name + "' did not register declared function '" +
                                       fn.name + "'");
                return false;
            }
        }
        for (const auto& w : p.manifest.widgets) {
            if (!widgetVtables_.count(w.name)) {
                closeLib(h);
                diag.error(Span{}, "Native plugin '" + name + "' did not register declared widget '" +
                                       w.name + "'");
                return false;
            }
        }
        p.dlHandle = h;
        p.bound = true;
    }
    return true;
}

bool PluginManager::hasNamespace(const std::string& ns) const {
    return namespaces_.count(ns) > 0;
}

const PluginFnInfo* PluginManager::findFunction(const std::string& qualifiedName) const {
    auto it = functions_.find(qualifiedName);
    return (it != functions_.end()) ? &it->second : nullptr;
}

namespace {
TransomValue toCValue(const Value& v, std::deque<std::string>& storage) {
    if (v.isInt()) return transom_int(v.asInt());
    if (v.isFloat()) return transom_float(v.asFloat());
    if (v.isBool()) return transom_bool(v.asBool() ? 1 : 0);
    if (v.isString()) {
        storage.push_back(v.asString());
        return TransomValue{TRANSOM_VAL_STRING, 0, 0.0, 0, storage.back().c_str()};
    }
    return transom_null();
}
} // namespace

Value PluginManager::call(const std::string& qualifiedName, const std::vector<Value>& args) {
    const PluginFnInfo* info = findFunction(qualifiedName);
    if (!info || !info->callback) return Value(Null{});

    std::deque<std::string> strStorage;
    std::vector<TransomValue> cArgs;
    cArgs.reserve(args.size());
    for (const Value& v : args) cArgs.push_back(toCValue(v, strStorage));

    TransomHostAPI api{};
    fillHostApi(api);

    size_t arenaBefore = stringArena_.size();
    TransomValue res = info->callback(&api, cArgs.data(), static_cast<int>(cArgs.size()));

    Value out;
    switch (res.kind) {
        case TRANSOM_VAL_INT: out = Value(res.i); break;
        case TRANSOM_VAL_FLOAT: out = Value(res.f); break;
        case TRANSOM_VAL_BOOL: out = Value(res.b != 0); break;
        case TRANSOM_VAL_STRING: out = Value(res.s ? std::string(res.s) : std::string()); break;
        default: out = Value(Null{}); break;
    }
    while (stringArena_.size() > arenaBefore) stringArena_.pop_back();
    return out;
}

bool PluginManager::hasNativeWidget(const std::string& kind) const {
    return widgetVtables_.count(kind) > 0;
}

void* PluginManager::createNativeWidget(const std::string& kind) {
    auto it = widgetVtables_.find(kind);
    if (it == widgetVtables_.end()) return nullptr;
    return it->second.create(it->second.plugin_ctx);
}

void PluginManager::setNativeWidgetProp(const std::string& kind, void* instance,
                                        const std::string& name, const Value& value) {
    auto it = widgetVtables_.find(kind);
    if (it == widgetVtables_.end() || !instance) return;
    std::deque<std::string> storage;
    TransomValue cv = toCValue(value, storage);
    it->second.set_prop(instance, name.c_str(), &cv);
}

void* PluginManager::nativeWidgetHandle(const std::string& kind, void* instance) {
    auto it = widgetVtables_.find(kind);
    if (it == widgetVtables_.end() || !instance) return nullptr;
    return it->second.native_handle(instance);
}

void PluginManager::destroyNativeWidget(const std::string& kind, void* instance) {
    auto it = widgetVtables_.find(kind);
    if (it == widgetVtables_.end() || !instance) return;
    eventSinks_.erase(instance);
    it->second.destroy(instance);
}

} // namespace transom
