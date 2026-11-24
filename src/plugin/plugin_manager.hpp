#pragma once

#include "common/source.hpp"
#include "common/value.hpp"
#include "platform/backend.hpp"
#include "plugin/native_manifest.hpp"
#include "sema/types.hpp"
#include "sema/widget_schema.hpp"
#include "transom_plugin.h"
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace transom {

struct PluginFnInfo {
    std::string qualifiedName; // e.g. "demo.add"
    std::vector<Type> params;
    Type returnType = Type::voidT();
    TransomFnCallback callback = nullptr; // bound at runtime (bindRuntime)
};

struct NativeLockInfo {
    std::string name;     // manifest name
    std::string version;
    std::string sha256;   // SHA-256 of the platform library file
    std::string path;     // library path
};

// Native plugin host. Two strictly separated phases:
//   1. loadPlugin()   COMPILE time: parses plugin.tnative only. Registers function
//                     signatures + widget schemas. Never loads native code.
//   2. bindRuntime()  RUNTIME: enforces the trust policy (SHA-256 in transom.lock),
//                     loads the library and binds callbacks/widget vtables.
class PluginManager : public NativeWidgetHost {
public:
    PluginManager();
    ~PluginManager() override;

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Phase 1 (compile time). `nameOrPath` is the `use "..."` string.
    bool loadPlugin(const std::string& nameOrPath,
                    const std::string& baseDir,
                    Span span,
                    DiagnosticEngine& diag);

    // Trust policy for native libraries. `lockedHashes` maps "native:<name>" to the
    // expected SHA-256. `trustAll` (dev only) skips the check.
    void setTrustPolicy(std::unordered_map<std::string, std::string> lockedHashes, bool trustAll);

    // Phase 2 (runtime): verify trust, dlopen, init, bind. Idempotent.
    bool bindRuntime(DiagnosticEngine& diag);

    std::vector<NativeLockInfo> nativeLockEntries() const;
    // (directory name, absolute-or-relative directory) of every loaded native plugin.
    std::vector<std::pair<std::string, std::string>> nativePluginDirs() const;

    bool hasNamespace(const std::string& ns) const;
    const PluginFnInfo* findFunction(const std::string& qualifiedName) const;
    const WidgetSchemaRegistry& widgets() const { return widgets_; }
    const std::string& lastEventName() const { return lastEventName_; }
    const std::string& lastEventPayload() const { return lastEventPayload_; }

    Value call(const std::string& qualifiedName, const std::vector<Value>& args);

    // NativeWidgetHost
    bool hasNativeWidget(const std::string& kind) const override;
    void* createNativeWidget(const std::string& kind) override;
    void setNativeWidgetProp(const std::string& kind, void* instance,
                             const std::string& name, const Value& value) override;
    void* nativeWidgetHandle(const std::string& kind, void* instance) override;
    void destroyNativeWidget(const std::string& kind, void* instance) override;
    void setNativeWidgetEventSink(
        const std::string& kind, void* instance,
        std::function<void(const std::string&, const Value&)> sink) override;

private:
    struct Loaded {
        NativeManifest manifest;
        std::string libraryPath; // resolved for this platform ("" if none)
        void* dlHandle = nullptr;
        bool bound = false;
    };

    std::vector<Loaded> plugins_;
    std::unordered_map<std::string, bool> namespaces_;
    std::unordered_map<std::string, PluginFnInfo> functions_;
    WidgetSchemaRegistry widgets_;
    std::unordered_map<std::string, TransomWidgetVTable> widgetVtables_;
    std::unordered_map<void*, std::function<void(const std::string&, const Value&)>> eventSinks_;
    std::unordered_map<std::string, std::string> lockedHashes_;
    bool trustAll_ = false;

    // Host context used during registration & call execution
    std::string activeNamespace_;
    std::string activeError_;
    std::string lastEventName_;
    std::string lastEventPayload_;
    std::deque<std::string> stringArena_;

    static void hostRegisterFn(void* hostCtx, const char* fnName, const TransomType* paramTypes,
                               int paramCount, TransomType returnType, TransomFnCallback cb);
    static const char* hostAllocString(void* hostCtx, const char* utf8, size_t len);
    static int hostRegisterWidget(void* hostCtx, const char* name, const TransomWidgetVTable* vt);
    static void hostSetError(void* hostCtx, const char* message);
    static void hostEmitWidgetEvent(void* hostCtx, void* instance, const char* event,
                                    const TransomValue* value);
    void fillHostApi(TransomHostAPI& api);
    static Type fromCAbiType(TransomType t);
};

} // namespace transom
