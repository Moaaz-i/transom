#pragma once

// Widget Schema Registry — COMPILE-TIME contract.
//
// Describes what the compiler needs to know about a widget: name, props and
// their types, positional prop, events, whether it takes children/actions.
// It contains NO runtime/native code. Built-in widgets and plugin widgets are
// registered through exactly the same structure (single source of truth for the
// type checker, compiler, IDE completions and generated documentation).

#include "sema/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace transom {

struct PropSpec {
    TypeKind type;
    bool requiresStateIdent = false;
};

struct WidgetSpec {
    bool allowsChildren = false;
    bool allowsAction = false;
    std::string positionalProp;
    std::unordered_map<std::string, PropSpec> props;
    // Filled by the registry / plugin manifest (not part of the builtin literals):
    std::vector<std::string> events{};  // declared events, e.g. "onPageChange"
    std::string ownerPlugin{};          // empty => built into the Transom core
    std::string name{};
};

class WidgetSchemaRegistry {
public:
    // Core built-in widgets only (no domain widgets such as PDF).
    static const WidgetSchemaRegistry& builtins();

    const WidgetSpec* find(const std::string& name) const {
        auto it = widgets_.find(name);
        return it == widgets_.end() ? nullptr : &it->second;
    }

    // Returns false (and leaves the registry unchanged) if the name is taken.
    bool add(WidgetSpec spec) {
        if (widgets_.count(spec.name)) return false;
        std::string key = spec.name;
        widgets_.emplace(std::move(key), std::move(spec));
        return true;
    }

    const std::unordered_map<std::string, WidgetSpec>& all() const { return widgets_; }

private:
    std::unordered_map<std::string, WidgetSpec> widgets_;
};

} // namespace transom
