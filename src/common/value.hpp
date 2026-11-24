#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace transom {

struct Null {};
struct Value;

using ValueList = std::shared_ptr<std::vector<Value>>;
using ValueBytes = std::shared_ptr<std::vector<uint8_t>>;
using ValueMap = std::shared_ptr<std::unordered_map<std::string, Value>>;

struct StructObject {
    std::string typeName;
    std::unordered_map<std::string, Value> fields;
};
using ValueStruct = std::shared_ptr<StructObject>;

struct Value {
    using Storage = std::variant<Null, int64_t, double, bool, std::string, ValueList, ValueStruct, ValueBytes, ValueMap>;
    Storage data = Null{};

    Value() = default;
    Value(Null) : data(Null{}) {}
    Value(int64_t v) : data(v) {}
    Value(int v) : data(static_cast<int64_t>(v)) {}
    Value(double v) : data(v) {}
    Value(bool v) : data(v) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(const char* v) : data(std::string(v)) {}
    Value(ValueList v) : data(std::move(v)) {}
    Value(ValueStruct v) : data(std::move(v)) {}
    Value(ValueBytes v) : data(std::move(v)) {}
    Value(ValueMap v) : data(std::move(v)) {}

    bool isNull() const { return std::holds_alternative<Null>(data); }
    bool isInt() const { return std::holds_alternative<int64_t>(data); }
    bool isFloat() const { return std::holds_alternative<double>(data); }
    bool isNumber() const { return isInt() || isFloat(); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isList() const { return std::holds_alternative<ValueList>(data); }
    bool isStruct() const { return std::holds_alternative<ValueStruct>(data); }
    bool isBytes() const { return std::holds_alternative<ValueBytes>(data); }
    bool isMap() const { return std::holds_alternative<ValueMap>(data); }

    int64_t asInt() const {
        if (isInt()) return std::get<int64_t>(data);
        if (isFloat()) return static_cast<int64_t>(std::get<double>(data));
        return 0;
    }

    double asFloat() const {
        if (isFloat()) return std::get<double>(data);
        if (isInt()) return static_cast<double>(std::get<int64_t>(data));
        return 0.0;
    }

    bool asBool() const {
        if (isBool()) return std::get<bool>(data);
        if (isNull()) return false;
        if (isInt()) return std::get<int64_t>(data) != 0;
        if (isFloat()) return std::get<double>(data) != 0.0;
        if (isString()) return !std::get<std::string>(data).empty();
        if (isList()) {
            const auto& l = std::get<ValueList>(data);
            return l && !l->empty();
        }
        if (isStruct()) return std::get<ValueStruct>(data) != nullptr;
        if (isMap()) { const auto& m = std::get<ValueMap>(data); return m && !m->empty(); }
        return false;
    }

    const std::string& asString() const {
        static const std::string kEmpty;
        if (isString()) return std::get<std::string>(data);
        return kEmpty;
    }

    ValueList asList() const {
        if (isList()) return std::get<ValueList>(data);
        return nullptr;
    }

    ValueStruct asStruct() const {
        if (isStruct()) return std::get<ValueStruct>(data);
        return nullptr;
    }

    ValueBytes asBytes() const {
        if (isBytes()) return std::get<ValueBytes>(data);
        return nullptr;
    }

    ValueMap asMap() const {
        if (isMap()) return std::get<ValueMap>(data);
        return nullptr;
    }

    std::string toString() const {
        if (isNull()) return "null";
        if (isInt()) return std::to_string(std::get<int64_t>(data));
        if (isFloat()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.12g", std::get<double>(data));
            return buf;
        }
        if (isBool()) return std::get<bool>(data) ? "true" : "false";
        if (isString()) return std::get<std::string>(data);
        if (isList()) {
            const auto& l = std::get<ValueList>(data);
            if (!l) return "[]";
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < l->size(); ++i) {
                if (i > 0) oss << ", ";
                oss << (*l)[i].toString();
            }
            oss << "]";
            return oss.str();
        }
        if (isStruct()) {
            const auto& s = std::get<ValueStruct>(data);
            if (!s) return "null";
            std::ostringstream oss;
            oss << s->typeName << "(";
            bool first = true;
            for (const auto& [k, v] : s->fields) {
                if (!first) oss << ", ";
                first = false;
                oss << k << ": " << v.toString();
            }
            oss << ")";
            return oss.str();
        }
        if (isBytes()) {
            const auto& b = std::get<ValueBytes>(data);
            return "Bytes(" + std::to_string(b ? b->size() : 0) + ")";
        }
        if (isMap()) {
            const auto& m = std::get<ValueMap>(data);
            return "Map(" + std::to_string(m ? m->size() : 0) + ")";
        }
        return "";
    }

    bool operator==(const Value& o) const {
        if (isNumber() && o.isNumber()) {
            if (isInt() && o.isInt()) return asInt() == o.asInt();
            return asFloat() == o.asFloat();
        }
        if (data.index() != o.data.index()) return false;
        if (isNull()) return true;
        if (isBool()) return asBool() == o.asBool();
        if (isString()) return asString() == o.asString();
        if (isList()) {
            auto a = asList(), b = o.asList();
            if (a == b) return true;
            if (!a || !b || a->size() != b->size()) return false;
            for (size_t i = 0; i < a->size(); ++i) {
                if ((*a)[i] != (*b)[i]) return false;
            }
            return true;
        }
        if (isStruct()) {
            auto a = asStruct(), b = o.asStruct();
            if (a == b) return true;
            if (!a || !b || a->typeName != b->typeName || a->fields.size() != b->fields.size())
                return false;
            for (const auto& [k, v] : a->fields) {
                auto it = b->fields.find(k);
                if (it == b->fields.end() || it->second != v) return false;
            }
            return true;
        }
        if (isBytes()) {
            auto a = asBytes(), b = o.asBytes();
            if (a == b) return true;
            if (!a || !b) return false;
            return *a == *b;
        }
        if (isMap()) {
            auto a = asMap(), b = o.asMap();
            if (a == b) return true;
            if (!a || !b) return false;
            return *a == *b;
        }
        return false;
    }

    bool operator!=(const Value& o) const { return !(*this == o); }
};

} // namespace transom
