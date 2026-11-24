#include "vm/serializer.hpp"

#include <cstring>
#include <fstream>

namespace transom {

namespace {

class BinWriter {
public:
    explicit BinWriter(std::ostream& os) : os_(os) {}

    void writeU8(uint8_t v) { os_.write(reinterpret_cast<const char*>(&v), 1); }
    void writeU16(uint16_t v) { os_.write(reinterpret_cast<const char*>(&v), 2); }
    void writeU32(uint32_t v) { os_.write(reinterpret_cast<const char*>(&v), 4); }
    void writeI64(int64_t v) { os_.write(reinterpret_cast<const char*>(&v), 8); }
    void writeF64(double v) { os_.write(reinterpret_cast<const char*>(&v), 8); }

    void writeString(const std::string& s) {
        writeU32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) os_.write(s.data(), static_cast<std::streamsize>(s.size()));
    }

    void writeValue(const Value& v) {
        if (v.isNull()) {
            writeU8(0);
        } else if (v.isInt()) {
            writeU8(1);
            writeI64(v.asInt());
        } else if (v.isFloat()) {
            writeU8(2);
            writeF64(v.asFloat());
        } else if (v.isBool()) {
            writeU8(3);
            writeU8(v.asBool() ? 1 : 0);
        } else if (v.isString()) {
            writeU8(4);
            writeString(v.asString());
        } else {
            writeU8(0);
        }
    }

    void writeChunk(const Chunk& c) {
        writeString(c.name);
        writeU32(static_cast<uint32_t>(c.code.size()));
        if (!c.code.empty()) {
            os_.write(reinterpret_cast<const char*>(c.code.data()),
                      static_cast<std::streamsize>(c.code.size()));
        }
        writeU32(static_cast<uint32_t>(c.constants.size()));
        for (const auto& val : c.constants) writeValue(val);
    }

    void writeExpr(const CompiledExpr& e) {
        writeChunk(e.chunk);
        writeU32(static_cast<uint32_t>(e.signalDeps.size()));
        for (const auto& d : e.signalDeps) writeString(d);
    }

    void writeProp(const CompiledProp& p) {
        writeString(p.name);
        writeString(p.bindState);
        writeExpr(p.expr);
    }

    void writeView(const CompiledView& v) {
        writeU8(static_cast<uint8_t>(v.kind));
        writeString(v.widget);
        writeU32(static_cast<uint32_t>(v.props.size()));
        for (const auto& p : v.props) writeProp(p);
        writeU32(static_cast<uint32_t>(v.children.size()));
        for (const auto& c : v.children) writeView(c);
        writeU8(v.hasAction ? 1 : 0);
        if (v.hasAction) writeChunk(v.actionChunk);

        if (v.kind == CompiledViewKind::If) {
            writeExpr(v.condition);
            writeU32(static_cast<uint32_t>(v.elseChildren.size()));
            for (const auto& c : v.elseChildren) writeView(c);
        } else if (v.kind == CompiledViewKind::For) {
            writeString(v.indexVar);
            writeString(v.itemVar);
            writeExpr(v.iterable);
        }
    }

    void writeStyleValue(const StyleValue& sv) {
        writeU8(static_cast<uint8_t>(sv.kind));
        switch (sv.kind) {
            case StyleValKind::Int: writeI64(sv.intVal); break;
            case StyleValKind::Float: writeF64(sv.floatVal); break;
            case StyleValKind::String:
            case StyleValKind::TokenRef: writeString(sv.strVal); break;
            case StyleValKind::Color:
                writeF64(sv.colorVal.r); writeF64(sv.colorVal.g);
                writeF64(sv.colorVal.b); writeF64(sv.colorVal.a);
                break;
        }
    }

    void writeStyleMap(const std::unordered_map<std::string, StyleValue>& m) {
        writeU32(static_cast<uint32_t>(m.size()));
        for (const auto& [k, v] : m) { writeString(k); writeStyleValue(v); }
    }

    void writeResolvedRule(const ResolvedStyleRule& r) {
        writeStyleMap(r.normalProps);
        writeStyleMap(r.hoverProps);
        writeStyleMap(r.pressedProps);
        writeStyleMap(r.disabledProps);
    }

    void writeStyleSheet(const CompiledStyleSheet& ss) {
        writeU32(ss.version);
        writeStyleMap(ss.defaultTokens);
        writeU32(static_cast<uint32_t>(ss.themes.size()));
        for (const auto& [tname, ov] : ss.themes) { writeString(tname); writeStyleMap(ov); }
        writeU32(static_cast<uint32_t>(ss.widgetDefaults.size()));
        for (const auto& [w, r] : ss.widgetDefaults) { writeString(w); writeResolvedRule(r); }
        writeU32(static_cast<uint32_t>(ss.classes.size()));
        for (const auto& [c, r] : ss.classes) { writeString(c); writeResolvedRule(r); }
    }

private:
    std::ostream& os_;
};

class BinReader {
public:
    explicit BinReader(std::istream& is) : is_(is) {}

    bool ok() const { return static_cast<bool>(is_); }

    uint8_t readU8() {
        uint8_t v = 0;
        is_.read(reinterpret_cast<char*>(&v), 1);
        return v;
    }
    uint32_t readU32() {
        uint32_t v = 0;
        is_.read(reinterpret_cast<char*>(&v), 4);
        return v;
    }
    int64_t readI64() {
        int64_t v = 0;
        is_.read(reinterpret_cast<char*>(&v), 8);
        return v;
    }
    double readF64() {
        double v = 0;
        is_.read(reinterpret_cast<char*>(&v), 8);
        return v;
    }

    std::string readString() {
        uint32_t len = readU32();
        std::string s(len, '\0');
        if (len > 0) is_.read(s.data(), static_cast<std::streamsize>(len));
        return s;
    }

    Value readValue() {
        uint8_t tag = readU8();
        switch (tag) {
            case 1: return Value(readI64());
            case 2: return Value(readF64());
            case 3: return Value(readU8() != 0);
            case 4: return Value(readString());
            default: return Value(Null{});
        }
    }

    Chunk readChunk() {
        Chunk c;
        c.name = readString();
        uint32_t codeLen = readU32();
        c.code.resize(codeLen);
        if (codeLen > 0) {
            is_.read(reinterpret_cast<char*>(c.code.data()),
                     static_cast<std::streamsize>(codeLen));
        }
        uint32_t constCount = readU32();
        c.constants.reserve(constCount);
        for (uint32_t i = 0; i < constCount; ++i) c.constants.push_back(readValue());
        return c;
    }

    CompiledExpr readExpr() {
        CompiledExpr e;
        e.chunk = readChunk();
        uint32_t n = readU32();
        e.signalDeps.reserve(n);
        for (uint32_t i = 0; i < n; ++i) e.signalDeps.push_back(readString());
        return e;
    }

    CompiledProp readProp() {
        CompiledProp p;
        p.name = readString();
        p.bindState = readString();
        p.expr = readExpr();
        return p;
    }

    CompiledView readView() {
        CompiledView v;
        v.kind = static_cast<CompiledViewKind>(readU8());
        v.widget = readString();
        uint32_t propCount = readU32();
        v.props.reserve(propCount);
        for (uint32_t i = 0; i < propCount; ++i) v.props.push_back(readProp());
        uint32_t childCount = readU32();
        v.children.reserve(childCount);
        for (uint32_t i = 0; i < childCount; ++i) v.children.push_back(readView());
        v.hasAction = (readU8() != 0);
        if (v.hasAction) v.actionChunk = readChunk();

        if (v.kind == CompiledViewKind::If) {
            v.condition = readExpr();
            uint32_t ec = readU32();
            v.elseChildren.reserve(ec);
            for (uint32_t i = 0; i < ec; ++i) v.elseChildren.push_back(readView());
        } else if (v.kind == CompiledViewKind::For) {
            v.indexVar = readString();
            v.itemVar = readString();
            v.iterable = readExpr();
        }
        return v;
    }

    StyleValue readStyleValue() {
        StyleValue sv;
        sv.kind = static_cast<StyleValKind>(readU8());
        switch (sv.kind) {
            case StyleValKind::Int: sv.intVal = readI64(); break;
            case StyleValKind::Float: sv.floatVal = readF64(); break;
            case StyleValKind::String:
            case StyleValKind::TokenRef: sv.strVal = readString(); break;
            case StyleValKind::Color:
                sv.colorVal.r = static_cast<float>(readF64());
                sv.colorVal.g = static_cast<float>(readF64());
                sv.colorVal.b = static_cast<float>(readF64());
                sv.colorVal.a = static_cast<float>(readF64());
                break;
        }
        return sv;
    }

    std::unordered_map<std::string, StyleValue> readStyleMap() {
        std::unordered_map<std::string, StyleValue> m;
        uint32_t n = readU32();
        for (uint32_t i = 0; i < n; ++i) {
            std::string k = readString();
            m[std::move(k)] = readStyleValue();
        }
        return m;
    }

    ResolvedStyleRule readResolvedRule() {
        ResolvedStyleRule r;
        r.normalProps = readStyleMap();
        r.hoverProps = readStyleMap();
        r.pressedProps = readStyleMap();
        r.disabledProps = readStyleMap();
        return r;
    }

    CompiledStyleSheet readStyleSheet() {
        CompiledStyleSheet ss;
        ss.version = readU32();
        ss.defaultTokens = readStyleMap();
        uint32_t tc = readU32();
        for (uint32_t i = 0; i < tc; ++i) {
            std::string tn = readString();
            ss.themes[std::move(tn)] = readStyleMap();
        }
        uint32_t wc = readU32();
        for (uint32_t i = 0; i < wc; ++i) {
            std::string wn = readString();
            ss.widgetDefaults[std::move(wn)] = readResolvedRule();
        }
        uint32_t cc = readU32();
        for (uint32_t i = 0; i < cc; ++i) {
            std::string cn = readString();
            ss.classes[std::move(cn)] = readResolvedRule();
        }
        return ss;
    }

private:
    std::istream& is_;
};

} // namespace

bool saveBytecodeFile(const CompiledProgram& program, const std::string& path) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    const char magic[4] = {'T', 'B', 'C', '2'};
    ofs.write(magic, 4);

    BinWriter w(ofs);
    w.writeU32(static_cast<uint32_t>(program.plugins.size()));
    for (const auto& p : program.plugins) w.writeString(p);
    w.writeStyleSheet(program.styleSheet);

    w.writeU32(static_cast<uint32_t>(program.pluginPermissions.size()));
    for (const auto& [pname, perms] : program.pluginPermissions) {
        w.writeString(pname);
        w.writeU32(static_cast<uint32_t>(perms.size()));
        for (const auto& perm : perms) w.writeString(perm);
    }

    w.writeU32(static_cast<uint32_t>(program.pluginOnLoad.size()));
    for (const auto& [pname, ch] : program.pluginOnLoad) {
        w.writeString(pname);
        w.writeChunk(ch);
    }

    w.writeU32(static_cast<uint32_t>(program.pluginOnUnload.size()));
    for (const auto& [pname, ch] : program.pluginOnUnload) {
        w.writeString(pname);
        w.writeChunk(ch);
    }

    w.writeU32(static_cast<uint32_t>(program.functions.size()));
    for (const auto& fn : program.functions) {
        w.writeString(fn.name);
        w.writeString(fn.ownerPlugin);
        w.writeU8(fn.arity);
        w.writeChunk(fn.chunk);
    }

    w.writeU8(program.hasApp ? 1 : 0);
    if (program.hasApp) {
        const auto& app = program.app;
        w.writeString(app.title);
        w.writeU32(static_cast<uint32_t>(app.windowProps.size()));
        for (const auto& wp : app.windowProps) w.writeProp(wp);
        w.writeU32(static_cast<uint32_t>(app.states.size()));
        for (const auto& st : app.states) {
            w.writeString(st.name);
            w.writeExpr(st.init);
            w.writeU8(st.isPersistent ? 1 : 0);
        }
        w.writeU32(static_cast<uint32_t>(app.deriveds.size()));
        for (const auto& dr : app.deriveds) {
            w.writeString(dr.name);
            w.writeExpr(dr.expr);
        }
        w.writeU32(static_cast<uint32_t>(app.timers.size()));
        for (const auto& tm : app.timers) {
            w.writeExpr(tm.intervalMs);
            w.writeChunk(tm.body);
        }
        w.writeView(app.rootView);
    }

    return static_cast<bool>(ofs);
}

std::optional<CompiledProgram> loadBytecodeFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return std::nullopt;

    char magic[4] = {};
    ifs.read(magic, 4);
    if (!ifs || std::memcmp(magic, "TBC2", 4) != 0) return std::nullopt;

    BinReader r(ifs);
    CompiledProgram prog;
    uint32_t plugCount = r.readU32();
    prog.plugins.reserve(plugCount);
    for (uint32_t i = 0; i < plugCount; ++i) prog.plugins.push_back(r.readString());
    prog.styleSheet = r.readStyleSheet();

    uint32_t permMapCount = r.readU32();
    for (uint32_t i = 0; i < permMapCount; ++i) {
        std::string pname = r.readString();
        uint32_t pc = r.readU32();
        std::vector<std::string> perms;
        perms.reserve(pc);
        for (uint32_t j = 0; j < pc; ++j) perms.push_back(r.readString());
        prog.pluginPermissions[std::move(pname)] = std::move(perms);
    }

    uint32_t onLoadCount = r.readU32();
    for (uint32_t i = 0; i < onLoadCount; ++i) {
        std::string pname = r.readString();
        prog.pluginOnLoad.emplace_back(std::move(pname), r.readChunk());
    }

    uint32_t onUnloadCount = r.readU32();
    for (uint32_t i = 0; i < onUnloadCount; ++i) {
        std::string pname = r.readString();
        prog.pluginOnUnload.emplace_back(std::move(pname), r.readChunk());
    }

    uint32_t fnCount = r.readU32();
    prog.functions.reserve(fnCount);
    for (uint32_t i = 0; i < fnCount; ++i) {
        CompiledFn fn;
        fn.name = r.readString();
        fn.ownerPlugin = r.readString();
        fn.arity = r.readU8();
        fn.chunk = r.readChunk();
        prog.functions.push_back(std::move(fn));
    }

    prog.hasApp = (r.readU8() != 0);
    if (prog.hasApp) {
        prog.app.title = r.readString();
        uint32_t wpCount = r.readU32();
        prog.app.windowProps.reserve(wpCount);
        for (uint32_t i = 0; i < wpCount; ++i) prog.app.windowProps.push_back(r.readProp());

        uint32_t stCount = r.readU32();
        prog.app.states.reserve(stCount);
        for (uint32_t i = 0; i < stCount; ++i) {
            CompiledState cs;
            cs.name = r.readString();
            cs.init = r.readExpr();
            cs.isPersistent = (r.readU8() != 0);
            prog.app.states.push_back(std::move(cs));
        }

        uint32_t drCount = r.readU32();
        prog.app.deriveds.reserve(drCount);
        for (uint32_t i = 0; i < drCount; ++i) {
            CompiledDerived cd;
            cd.name = r.readString();
            cd.expr = r.readExpr();
            prog.app.deriveds.push_back(std::move(cd));
        }

        uint32_t tmCount = r.readU32();
        prog.app.timers.reserve(tmCount);
        for (uint32_t i = 0; i < tmCount; ++i) {
            CompiledTimer ct;
            ct.intervalMs = r.readExpr();
            ct.body = r.readChunk();
            prog.app.timers.push_back(std::move(ct));
        }

        prog.app.rootView = r.readView();
    }

    if (!r.ok()) return std::nullopt;
    return prog;
}

} // namespace transom
