#include "plugin/native_manifest.hpp"

#include <cctype>
#include <filesystem>

namespace transom {

namespace {

struct Tok {
    enum Kind { Ident, Str, Sym, End } kind = End;
    std::string text;
    int line = 1;
};

std::vector<Tok> tokenize(const std::string& s) {
    std::vector<Tok> out;
    int line = 1;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '"') {
            std::string v;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                if (s[i] == '\n') ++line;
                v.push_back(s[i++]);
            }
            ++i;
            out.push_back({Tok::Str, v, line});
            continue;
        }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') {
            out.push_back({Tok::Sym, "->", line});
            i += 2;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            std::string v;
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '.')) {
                v.push_back(s[i++]);
            }
            out.push_back({Tok::Ident, v, line});
            continue;
        }
        out.push_back({Tok::Sym, std::string(1, c), line});
        ++i;
    }
    out.push_back({Tok::End, "", line});
    return out;
}

class ManifestParser {
public:
    ManifestParser(std::vector<Tok> toks, const std::string& path, DiagnosticEngine& diag)
        : toks_(std::move(toks)), path_(path), diag_(diag) {}

    bool parse(NativeManifest& m) {
        if (!word("native") || !word("plugin")) return fail("expected 'native plugin \"name\" {'");
        if (cur().kind != Tok::Str) return fail("expected plugin name string");
        m.name = take().text;
        m.ns = m.name;
        for (char& c : m.ns) if (c == '-') c = '_';
        if (!sym("{")) return fail("expected '{'");
        while (!isSym("}")) {
            if (cur().kind != Tok::Ident) return fail("unexpected token '" + cur().text + "'");
            std::string key = take().text;
            if (key == "version") {
                if (!sym(":") || cur().kind != Tok::Str) return fail("version: \"x.y.z\" expected");
                m.version = take().text;
            } else if (key == "api") {
                if (!sym(":") || cur().kind != Tok::Ident) return fail("api: <number> expected");
                m.api = std::atoi(take().text.c_str());
            } else if (key == "library") {
                if (!sym("{")) return fail("expected '{' after library");
                while (!isSym("}")) {
                    if (cur().kind != Tok::Ident) return fail("platform name expected");
                    std::string plat = take().text;
                    if (!sym(":") || cur().kind != Tok::Str) return fail("platform: \"path\" expected");
                    m.libraries[plat] = take().text;
                }
                sym("}");
            } else if (key == "fn") {
                if (!parseFn(m)) return false;
            } else if (key == "widget") {
                if (!parseWidget(m)) return false;
            } else {
                return fail("unknown manifest key '" + key + "'");
            }
        }
        sym("}");
        if (m.version.empty()) return fail("missing 'version'");
        if (m.api == 0) return fail("missing 'api' (native plugin ABI version)");
        if (m.libraries.empty()) return fail("missing 'library { ... }'");
        return !failed_;
    }

private:
    std::vector<Tok> toks_;
    size_t pos_ = 0;
    std::string path_;
    DiagnosticEngine& diag_;
    bool failed_ = false;

    const Tok& cur() const { return toks_[pos_]; }
    Tok take() { Tok t = toks_[pos_]; if (pos_ + 1 < toks_.size()) ++pos_; return t; }
    bool isSym(const char* s) const { return cur().kind == Tok::Sym && cur().text == s; }
    bool sym(const char* s) { if (isSym(s)) { take(); return true; } return false; }
    bool word(const char* w) {
        if (cur().kind == Tok::Ident && cur().text == w) { take(); return true; }
        return false;
    }
    bool fail(const std::string& msg) {
        if (!failed_) {
            diag_.error(Span{}, path_ + ":" + std::to_string(cur().line) + ": " + msg);
        }
        failed_ = true;
        return false;
    }

    bool toType(const std::string& n, Type& t) {
        if (n == "Int") t = Type::intT();
        else if (n == "Float") t = Type::floatT();
        else if (n == "Bool") t = Type::boolT();
        else if (n == "String") t = Type::stringT();
        else if (n == "Void") t = Type::voidT();
        else if (n == "Any") t = Type::anyT();
        else return false;
        return true;
    }

    bool parseFn(NativeManifest& m) {
        if (cur().kind != Tok::Ident) return fail("function name expected");
        NativeFnDecl fn;
        fn.name = take().text;
        if (!sym("(")) return fail("expected '('");
        while (!isSym(")")) {
            if (cur().kind != Tok::Ident) return fail("parameter name expected");
            take();
            if (!sym(":") || cur().kind != Tok::Ident) return fail("parameter type expected");
            Type t;
            std::string tn = take().text;
            if (!toType(tn, t) || t.kind == TypeKind::Void) return fail("unsupported parameter type '" + tn + "'");
            fn.params.push_back(t);
            sym(",");
        }
        sym(")");
        if (sym("->")) {
            if (cur().kind != Tok::Ident) return fail("return type expected");
            std::string tn = take().text;
            if (!toType(tn, fn.returnType)) return fail("unsupported return type '" + tn + "'");
        }
        m.fns.push_back(std::move(fn));
        return true;
    }

    bool propType(const std::string& n, TypeKind& k) {
        if (n == "Int") k = TypeKind::Int;
        else if (n == "Float") k = TypeKind::Float;
        else if (n == "Bool") k = TypeKind::Bool;
        else if (n == "String") k = TypeKind::String;
        else if (n == "Any") k = TypeKind::Any;
        else return false;
        return true;
    }

    bool parseWidget(NativeManifest& m) {
        if (cur().kind != Tok::Ident) return fail("widget name expected");
        WidgetSpec w;
        w.name = take().text;
        w.ownerPlugin = m.name;
        if (!sym("{")) return fail("expected '{'");
        while (!isSym("}")) {
            if (cur().kind != Tok::Ident) return fail("unexpected token in widget '" + w.name + "'");
            std::string key = take().text;
            if (key == "positional") {
                if (!sym(":") || cur().kind != Tok::Ident) return fail("positional: <prop> expected");
                w.positionalProp = take().text;
            } else if (key == "prop") {
                if (cur().kind != Tok::Ident) return fail("prop name expected");
                std::string pn = take().text;
                if (!sym(":") || cur().kind != Tok::Ident) return fail("prop type expected");
                std::string tn = take().text;
                TypeKind k;
                if (!propType(tn, k)) return fail("unsupported prop type '" + tn + "'");
                PropSpec ps{k};
                if (cur().kind == Tok::Ident && cur().text == "bind") {
                    take();
                    ps.requiresStateIdent = true;
                }
                w.props[pn] = ps;
            } else if (key == "event") {
                if (cur().kind != Tok::Ident) return fail("event name expected");
                w.events.push_back(take().text);
            } else {
                return fail("unknown widget key '" + key + "'");
            }
        }
        sym("}");
        // Universal layout props every native widget supports through the backend.
        if (!w.props.count("width")) w.props["width"] = PropSpec{TypeKind::Int};
        if (!w.props.count("height")) w.props["height"] = PropSpec{TypeKind::Int};
        if (!w.props.count("opacity")) w.props["opacity"] = PropSpec{TypeKind::Float};
        w.allowsAction = !w.events.empty();
        if (!w.positionalProp.empty() && !w.props.count(w.positionalProp)) {
            return fail("positional prop '" + w.positionalProp + "' is not declared in widget '" + w.name + "'");
        }
        m.widgets.push_back(std::move(w));
        return true;
    }
};

} // namespace

bool parseNativeManifest(const std::string& source,
                         const std::string& manifestPath,
                         NativeManifest& out,
                         DiagnosticEngine& diag) {
    ManifestParser p(tokenize(source), manifestPath, diag);
    out = NativeManifest{};
    out.manifestPath = manifestPath;
    out.dir = std::filesystem::path(manifestPath).parent_path().string();
    return p.parse(out);
}

} // namespace transom
