#pragma once

#include "common/value.hpp"
#include "platform/backend.hpp"
#include "plugin/plugin_manager.hpp"
#include "reactive/signal_graph.hpp"
#include "vm/bytecode.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace transom {

using ScopeMap = std::unordered_map<std::string, Value>;
using PermissionMap = std::unordered_map<std::string, std::vector<std::string>>;

class VM {
public:
    VM(const std::vector<CompiledFn>& functions,
       SignalGraph& signals,
       PlatformBackend* backend = nullptr,
       std::string appName = "Transom",
       PluginManager* plugins = nullptr,
       const PermissionMap* pluginPermissions = nullptr);

    Value execute(const Chunk& chunk,
                  const std::vector<Value>& args = {},
                  const ScopeMap* captured = nullptr,
                  const std::string& ownerPlugin = "");

    std::function<void(const std::string&)> onPrint;
    std::function<bool(const std::string&)> onSetTheme;

private:
    const std::vector<CompiledFn>& functions_;
    SignalGraph& signals_;
    PlatformBackend* backend_ = nullptr;
    std::string appName_;
    PluginManager* plugins_ = nullptr;
    const PermissionMap* pluginPermissions_ = nullptr;
    std::vector<std::string> pluginCallStack_;

    const std::string& currentPlugin() const;
    void enforceRuntimePermission(const std::string& requiredPerm,
                                  const std::string& opName,
                                  bool requirePluginContext = false) const;
    Value callBuiltin(int id, const std::vector<Value>& args);
};

} // namespace transom
