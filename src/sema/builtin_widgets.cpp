#include "sema/widget_schema.hpp"

namespace transom {

namespace {

std::unordered_map<std::string, PropSpec> flexLayoutProps() {
    return {
        {"width", {TypeKind::Int}},
        {"height", {TypeKind::Int}},
        {"minWidth", {TypeKind::Int}},
        {"maxWidth", {TypeKind::Int}},
        {"minHeight", {TypeKind::Int}},
        {"maxHeight", {TypeKind::Int}},
        {"grow", {TypeKind::Float}},
        {"shrink", {TypeKind::Float}},
        {"opacity", {TypeKind::Float}},
    };
}

std::unordered_map<std::string, PropSpec> mergeProps(
    std::unordered_map<std::string, PropSpec> base,
    std::initializer_list<std::pair<std::string, PropSpec>> extra) {
    for (const auto& kv : extra) base[kv.first] = kv.second;
    return base;
}

const std::unordered_map<std::string, WidgetSpec>& builtinWidgetTable() {
    static const std::unordered_map<std::string, WidgetSpec> kWidgets = {
        {"VStack", {true, false, "", mergeProps(flexLayoutProps(), {
            {"gap", {TypeKind::Int}},
            {"padding", {TypeKind::Int}},
            {"align", {TypeKind::String}},
            {"justify", {TypeKind::String}},
            {"rtl", {TypeKind::Bool}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
        })}},
        {"HStack", {true, false, "", mergeProps(flexLayoutProps(), {
            {"gap", {TypeKind::Int}},
            {"padding", {TypeKind::Int}},
            {"align", {TypeKind::String}},
            {"justify", {TypeKind::String}},
            {"rtl", {TypeKind::Bool}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
        })}},
        {"Grid", {true, false, "", mergeProps(flexLayoutProps(), {
            {"columns", {TypeKind::Int}},
            {"gap", {TypeKind::Int}},
            {"padding", {TypeKind::Int}},
            {"align", {TypeKind::String}},
            {"rtl", {TypeKind::Bool}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
        })}},
        {"ZStack", {true, false, "", mergeProps(flexLayoutProps(), {
            {"padding", {TypeKind::Int}},
            {"align", {TypeKind::String}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
        })}},
        {"Modal", {true, false, "", mergeProps(flexLayoutProps(), {
            {"open", {TypeKind::Bool}},
            {"padding", {TypeKind::Int}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
        })}},
        {"ScrollView", {true, false, "", mergeProps(flexLayoutProps(), {
            {"gap", {TypeKind::Int}},
            {"padding", {TypeKind::Int}},
        })}},
        {"Card", {true, false, "", mergeProps(flexLayoutProps(), {
            {"padding", {TypeKind::Int}},
            {"bg", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
            {"gap", {TypeKind::Int}},
            {"align", {TypeKind::String}},
            {"justify", {TypeKind::String}},
            {"shadow", {TypeKind::Int}},
        })}},
        {"Spacer", {false, false, "", {
            {"size", {TypeKind::Int}},
            {"grow", {TypeKind::Float}},
        }}},
        {"Divider", {false, false, "", {
            {"width", {TypeKind::Int}},
            {"color", {TypeKind::String}},
        }}},
        {"Icon", {false, false, "name", {
            {"name", {TypeKind::String}},
            {"size", {TypeKind::Int}},
            {"color", {TypeKind::String}},
            {"grow", {TypeKind::Float}},
        }}},
        {"Text", {false, false, "text", mergeProps(flexLayoutProps(), {
            {"text", {TypeKind::String}},
            {"size", {TypeKind::Int}},
            {"weight", {TypeKind::String}},
            {"color", {TypeKind::String}},
            {"align", {TypeKind::String}},
        })}},
        {"Button", {false, true, "label", mergeProps(flexLayoutProps(), {
            {"label", {TypeKind::String}},
            {"icon", {TypeKind::String}},
            {"style", {TypeKind::String}},
            {"bg", {TypeKind::String}},
            {"color", {TypeKind::String}},
            {"radius", {TypeKind::Int}},
            {"size", {TypeKind::Int}},
            {"weight", {TypeKind::String}},
            {"padding", {TypeKind::Int}},
        })}},
        {"Input", {false, false, "", mergeProps(flexLayoutProps(), {
            {"bind", {TypeKind::String, true}},
            {"placeholder", {TypeKind::String}},
        })}},
        {"Slider", {false, false, "", mergeProps(flexLayoutProps(), {
            {"bind", {TypeKind::Float, true}},
            {"min", {TypeKind::Float}},
            {"max", {TypeKind::Float}},
        })}},
        {"Toggle", {false, false, "label", {
            {"label", {TypeKind::String}},
            {"bind", {TypeKind::Bool, true}},
            {"grow", {TypeKind::Float}},
        }}},
        {"Select", {false, false, "", mergeProps(flexLayoutProps(), {
            {"options", {TypeKind::List}},
            {"bind", {TypeKind::String, true}},
        })}},
        {"Image", {false, false, "src", mergeProps(flexLayoutProps(), {
            {"src", {TypeKind::String}},
        })}},
        {"Progress", {false, false, "value", mergeProps(flexLayoutProps(), {
            {"value", {TypeKind::Float}},
            {"max", {TypeKind::Float}},
        })}},
        {"Canvas", {false, false, "", mergeProps(flexLayoutProps(), {
            {"bg", {TypeKind::String}},
            {"draw", {TypeKind::String}},
            {"interactive", {TypeKind::Bool}},
            {"strokeColor", {TypeKind::String}},
            {"strokeWidth", {TypeKind::Float}},
        })}},
    };
    return kWidgets;
}

} // namespace

const WidgetSchemaRegistry& WidgetSchemaRegistry::builtins() {
    static const WidgetSchemaRegistry kBuiltins = [] {
        WidgetSchemaRegistry reg;
        for (const auto& [name, spec] : builtinWidgetTable()) {
            WidgetSpec copy = spec;
            copy.name = name;
            reg.widgets_[name] = std::move(copy);
        }
        return reg;
    }();
    return kBuiltins;
}

} // namespace transom
