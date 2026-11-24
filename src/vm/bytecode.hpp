#pragma once

#include "common/value.hpp"
#include "style/stylesheet.hpp"
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace transom {

enum class OpCode : uint8_t {
    Const,       // [u16 constIdx]
    Null,
    True,
    False,
    Pop,

    GetLocal,    // [u16 slot]
    SetLocal,    // [u16 slot]
    GetSignal,   // [u16 nameConstIdx]
    SetSignal,   // [u16 nameConstIdx]
    NotifySignal,// [u16 nameConstIdx]

    MakeList,    // [u16 count]
    MakeStruct,  // [u16 typeNameConstIdx, u16 fieldCount] (stack: name0, val0, ...)
    GetIndex,    // stack: [list, idx] -> [val]
    SetIndex,    // stack: [list, idx, val] -> []
    GetField,    // [u16 fieldNameConstIdx]  stack: [struct] -> [val]
    SetField,    // [u16 fieldNameConstIdx]  stack: [struct, val] -> []

    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    Eq,
    Neq,
    Lt,
    Le,
    Gt,
    Ge,

    ToString,
    Concat,      // [u16 count]

    Jump,        // [u16 targetIp]
    JumpIfFalse, // [u16 targetIp]

    Call,        // [u16 fnIdx, u8 argc]
    CallBuiltin, // [u16 builtinIdx, u8 argc]
    CallPlugin,  // [u16 qualifiedNameConstIdx, u8 argc]
    Return
};

struct Chunk {
    std::string name;
    std::vector<uint8_t> code;
    std::vector<Value> constants;

    uint16_t addConstant(Value v) {
        constants.push_back(std::move(v));
        return static_cast<uint16_t>(constants.size() - 1);
    }

    void emit(OpCode op) {
        code.push_back(static_cast<uint8_t>(op));
    }

    void emitU8(uint8_t v) {
        code.push_back(v);
    }

    void emitU16(uint16_t v) {
        code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        code.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    size_t emitJump(OpCode op) {
        emit(op);
        size_t patchPos = code.size();
        emitU16(0);
        return patchPos;
    }

    void patchJump(size_t patchPos) {
        uint16_t target = static_cast<uint16_t>(code.size());
        code[patchPos] = static_cast<uint8_t>((target >> 8) & 0xFF);
        code[patchPos + 1] = static_cast<uint8_t>(target & 0xFF);
    }

    uint16_t readU16(size_t ip) const {
        return static_cast<uint16_t>((static_cast<uint16_t>(code[ip]) << 8) | code[ip + 1]);
    }

    std::string disassemble() const {
        std::ostringstream oss;
        oss << "== " << name << " ==\n";
        size_t ip = 0;
        while (ip < code.size()) {
            char addr[16];
            std::snprintf(addr, sizeof(addr), "%04zu  ", ip);
            oss << addr;
            OpCode op = static_cast<OpCode>(code[ip++]);
            switch (op) {
                case OpCode::Const: {
                    uint16_t idx = readU16(ip); ip += 2;
                    oss << "CONST        #" << idx << " (" << constants[idx].toString() << ")\n";
                    break;
                }
                case OpCode::Null: oss << "NULL\n"; break;
                case OpCode::True: oss << "TRUE\n"; break;
                case OpCode::False: oss << "FALSE\n"; break;
                case OpCode::Pop: oss << "POP\n"; break;
                case OpCode::GetLocal: {
                    uint16_t s = readU16(ip); ip += 2;
                    oss << "GET_LOCAL    " << s << "\n";
                    break;
                }
                case OpCode::SetLocal: {
                    uint16_t s = readU16(ip); ip += 2;
                    oss << "SET_LOCAL    " << s << "\n";
                    break;
                }
                case OpCode::GetSignal: {
                    uint16_t idx = readU16(ip); ip += 2;
                    oss << "GET_SIGNAL   " << constants[idx].toString() << "\n";
                    break;
                }
                case OpCode::SetSignal: {
                    uint16_t idx = readU16(ip); ip += 2;
                    oss << "SET_SIGNAL   " << constants[idx].toString() << "\n";
                    break;
                }
                case OpCode::NotifySignal: {
                    uint16_t idx = readU16(ip); ip += 2;
                    oss << "NOTIFY_SIG   " << constants[idx].toString() << "\n";
                    break;
                }
                case OpCode::MakeList: {
                    uint16_t n = readU16(ip); ip += 2;
                    oss << "MAKE_LIST    " << n << "\n";
                    break;
                }
                case OpCode::MakeStruct: {
                    uint16_t t = readU16(ip); ip += 2;
                    uint16_t n = readU16(ip); ip += 2;
                    oss << "MAKE_STRUCT  " << constants[t].toString() << " (" << n << " fields)\n";
                    break;
                }
                case OpCode::GetIndex: oss << "GET_INDEX\n"; break;
                case OpCode::SetIndex: oss << "SET_INDEX\n"; break;
                case OpCode::GetField: {
                    uint16_t f = readU16(ip); ip += 2;
                    oss << "GET_FIELD    ." << constants[f].toString() << "\n";
                    break;
                }
                case OpCode::SetField: {
                    uint16_t f = readU16(ip); ip += 2;
                    oss << "SET_FIELD    ." << constants[f].toString() << "\n";
                    break;
                }
                case OpCode::Add: oss << "ADD\n"; break;
                case OpCode::Sub: oss << "SUB\n"; break;
                case OpCode::Mul: oss << "MUL\n"; break;
                case OpCode::Div: oss << "DIV\n"; break;
                case OpCode::Mod: oss << "MOD\n"; break;
                case OpCode::Neg: oss << "NEG\n"; break;
                case OpCode::Not: oss << "NOT\n"; break;
                case OpCode::Eq: oss << "EQ\n"; break;
                case OpCode::Neq: oss << "NEQ\n"; break;
                case OpCode::Lt: oss << "LT\n"; break;
                case OpCode::Le: oss << "LE\n"; break;
                case OpCode::Gt: oss << "GT\n"; break;
                case OpCode::Ge: oss << "GE\n"; break;
                case OpCode::ToString: oss << "TO_STRING\n"; break;
                case OpCode::Concat: {
                    uint16_t n = readU16(ip); ip += 2;
                    oss << "CONCAT       " << n << "\n";
                    break;
                }
                case OpCode::Jump: {
                    uint16_t t = readU16(ip); ip += 2;
                    oss << "JUMP         ->" << t << "\n";
                    break;
                }
                case OpCode::JumpIfFalse: {
                    uint16_t t = readU16(ip); ip += 2;
                    oss << "JUMP_FALSE   ->" << t << "\n";
                    break;
                }
                case OpCode::Call: {
                    uint16_t fn = readU16(ip); ip += 2;
                    uint8_t argc = code[ip++];
                    oss << "CALL         fn#" << fn << " (" << static_cast<int>(argc) << " args)\n";
                    break;
                }
                case OpCode::CallBuiltin: {
                    uint16_t b = readU16(ip); ip += 2;
                    uint8_t argc = code[ip++];
                    oss << "BUILTIN      #" << b << " (" << static_cast<int>(argc) << " args)\n";
                    break;
                }
                case OpCode::CallPlugin: {
                    uint16_t idx = readU16(ip); ip += 2;
                    uint8_t argc = code[ip++];
                    oss << "CALL_PLUGIN  " << constants[idx].toString()
                        << " (" << static_cast<int>(argc) << " args)\n";
                    break;
                }
                case OpCode::Return: oss << "RETURN\n"; break;
            }
        }
        return oss.str();
    }
};

struct CompiledFn {
    std::string name;
    std::string ownerPlugin; // empty for app/user functions; plugin name for .tplug functions
    uint8_t arity = 0;
    Chunk chunk;
};

struct CompiledExpr {
    Chunk chunk;
    std::vector<std::string> signalDeps;
};

enum class CompiledViewKind {
    Widget,
    If,
    For
};

struct CompiledProp {
    std::string name;
    std::string bindState;
    CompiledExpr expr;
};

struct CompiledView {
    CompiledViewKind kind = CompiledViewKind::Widget;

    // Widget fields
    std::string widget;
    std::vector<CompiledProp> props;
    std::vector<CompiledView> children;
    bool hasAction = false;
    Chunk actionChunk;

    // IfView fields
    CompiledExpr condition;
    std::vector<CompiledView> elseChildren;

    // ForView fields
    std::string indexVar;
    std::string itemVar;
    CompiledExpr iterable;
};

struct CompiledState {
    std::string name;
    CompiledExpr init;
    bool isPersistent = false;
};

struct CompiledDerived {
    std::string name;
    CompiledExpr expr;
};

struct CompiledTimer {
    CompiledExpr intervalMs;
    Chunk body;
};

struct CompiledApp {
    std::string title;
    std::vector<CompiledProp> windowProps;
    std::vector<CompiledState> states;
    std::vector<CompiledDerived> deriveds;
    std::vector<CompiledTimer> timers;
    CompiledView rootView;
};

struct CompiledProgram {
    std::vector<std::string> plugins; // native .dylib plugins
    CompiledStyleSheet styleSheet;
    std::unordered_map<std::string, std::vector<std::string>> pluginPermissions;
    std::vector<std::pair<std::string, Chunk>> pluginOnLoad;
    std::vector<std::pair<std::string, Chunk>> pluginOnUnload;
    std::vector<CompiledFn> functions;
    bool hasApp = false;
    CompiledApp app;

    std::string disassemble() const {
        std::ostringstream oss;
        for (const auto& fn : functions) {
            oss << fn.chunk.disassemble() << "\n";
        }
        if (hasApp) {
            for (const auto& st : app.states) {
                oss << st.init.chunk.disassemble() << "\n";
            }
            for (const auto& dr : app.deriveds) {
                oss << dr.expr.chunk.disassemble() << "\n";
            }
        }
        return oss.str();
    }
};

} // namespace transom
