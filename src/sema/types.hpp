#pragma once

#include <memory>
#include <string>

namespace transom {

enum class TypeKind {
    Error,
    Void,
    Int,
    Float,
    Bool,
    String,
    Bytes,
    Any,
    List,
    Map,
    Result,
    Struct,
    Enum
};

struct Type {
    TypeKind kind = TypeKind::Error;
    std::shared_ptr<Type> element; // for List<T>
    std::string structName;        // for Struct

    static Type primitive(TypeKind k) { return Type{k, nullptr, ""}; }
    static Type error() { return primitive(TypeKind::Error); }
    static Type voidT() { return primitive(TypeKind::Void); }
    static Type intT() { return primitive(TypeKind::Int); }
    static Type floatT() { return primitive(TypeKind::Float); }
    static Type boolT() { return primitive(TypeKind::Bool); }
    static Type stringT() { return primitive(TypeKind::String); }
    static Type bytesT() { return primitive(TypeKind::Bytes); }
    static Type anyT() { return primitive(TypeKind::Any); }

    static Type listOf(Type elem) {
        return Type{TypeKind::List, std::make_shared<Type>(std::move(elem)), ""};
    }

    static Type mapOf(Type elem) {
        return Type{TypeKind::Map, std::make_shared<Type>(std::move(elem)), ""};
    }

    static Type resultOf(Type elem) {
        return Type{TypeKind::Result, std::make_shared<Type>(std::move(elem)), ""};
    }

    static Type enumOf(std::string name) {
        return Type{TypeKind::Enum, nullptr, std::move(name)};
    }

    static Type structOf(std::string name) {
        return Type{TypeKind::Struct, nullptr, std::move(name)};
    }

    bool isError() const { return kind == TypeKind::Error; }
    bool isNumeric() const { return kind == TypeKind::Int || kind == TypeKind::Float; }

    std::string name() const {
        switch (kind) {
            case TypeKind::Error: return "<error>";
            case TypeKind::Void: return "Void";
            case TypeKind::Int: return "Int";
            case TypeKind::Float: return "Float";
            case TypeKind::Bool: return "Bool";
            case TypeKind::String: return "String";
            case TypeKind::Bytes: return "Bytes";
            case TypeKind::Any: return "Any";
            case TypeKind::List:
                return "List<" + (element ? element->name() : "Any") + ">";
            case TypeKind::Map:
                return "Map<String, " + (element ? element->name() : "Any") + ">";
            case TypeKind::Result:
                return "Result<" + (element ? element->name() : "Any") + ">";
            case TypeKind::Struct:
            case TypeKind::Enum:
                return structName;
        }
        return "?";
    }

    bool operator==(const Type& o) const {
        if (kind != o.kind) return false;
        if (kind == TypeKind::List || kind == TypeKind::Map || kind == TypeKind::Result) {
            if (!element || !o.element) return true;
            if (element->kind == TypeKind::Any || o.element->kind == TypeKind::Any) return true;
            return *element == *o.element;
        }
        if (kind == TypeKind::Struct || kind == TypeKind::Enum) return structName == o.structName;
        return true;
    }

    bool operator!=(const Type& o) const { return !(*this == o); }
};

inline bool isAssignable(const Type& target, const Type& source) {
    if (target.isError() || source.isError()) return true;
    if (target.kind == TypeKind::Any || source.kind == TypeKind::Any) return true;
    if (target.kind == TypeKind::Float && source.kind == TypeKind::Int) return true;
    if ((target.kind == TypeKind::List && source.kind == TypeKind::List) ||
        (target.kind == TypeKind::Map && source.kind == TypeKind::Map) ||
        (target.kind == TypeKind::Result && source.kind == TypeKind::Result)) {
        if (!target.element || !source.element) return true;
        return isAssignable(*target.element, *source.element);
    }
    return target == source;
}

} // namespace transom
