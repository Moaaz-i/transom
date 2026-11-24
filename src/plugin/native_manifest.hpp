#pragma once

// plugin.tnative — compile-time manifest of a NATIVE plugin.
// Parsed without loading any native code.

#include "common/source.hpp"
#include "sema/types.hpp"
#include "sema/widget_schema.hpp"
#include <map>
#include <string>
#include <vector>

namespace transom {

struct NativeFnDecl {
    std::string name;
    std::vector<Type> params;
    Type returnType = Type::voidT();
};

struct NativeManifest {
    std::string name;       // as written in the manifest, e.g. "pdf-native"
    std::string ns;         // Transom namespace (name with '-' => '_')
    std::string version;
    int api = 0;
    std::map<std::string, std::string> libraries; // platform -> relative path
    std::vector<NativeFnDecl> fns;
    std::vector<WidgetSpec> widgets;
    std::string manifestPath;
    std::string dir;        // directory containing the manifest
};

// Returns false and reports through `diag` on malformed manifests.
bool parseNativeManifest(const std::string& source,
                         const std::string& manifestPath,
                         NativeManifest& out,
                         DiagnosticEngine& diag);

} // namespace transom
