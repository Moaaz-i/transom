#pragma once

#include "platform/backend.hpp"
#include "plugin/plugin_manager.hpp"
#include "reactive/signal_graph.hpp"
#include "style/stylesheet.hpp"
#include "vm/bytecode.hpp"
#include "vm/vm.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transom {

// Connects the Bytecode VM, Reactive SignalGraph, PluginManager, Style Runtime
// (incremental Theme reconciliation), and PlatformBackend into a live app.
class AppRuntime {
public:
    explicit AppRuntime(PlatformBackend& backend, PluginManager* plugins = nullptr);
    ~AppRuntime();

    void mount(CompiledProgram program);
    void reload(CompiledProgram program);

    // Hot-reload only the .tstyle stylesheet WITHOUT rebuilding views or losing state!
    void reloadStyle(CompiledStyleSheet newSheet);

    // Incrementally switch theme (returns false without changing theme if unknown).
    bool setTheme(const std::string& themeName);
    const std::string& activeTheme() const { return activeTheme_; }

    SignalGraph& signals() { return signals_; }
    VM& vm() { return *vm_; }

private:
    struct StyledWidgetRecord {
        WidgetHandle handle = kInvalidWidget;
        std::string widgetKind;
        std::vector<std::string> classes;
        std::unordered_set<std::string> inlineProps;
        std::unordered_map<std::string, Value> lastApplied;
    };

    PlatformBackend& backend_;
    PluginManager* plugins_ = nullptr;
    CompiledProgram program_;
    SignalGraph signals_;
    std::unique_ptr<VM> vm_;

    std::string activeTheme_ = "default";
    std::vector<StyledWidgetRecord> styledWidgets_;

    WidgetHandle buildView(const CompiledView& node, const ScopeMap& captured);
    void applyResolvedStyles(StyledWidgetRecord& rec);
    void reconcileAllStyles();
};

} // namespace transom
