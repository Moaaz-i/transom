#include "vm/vm.hpp"
#include "stdlib/system_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace transom {

VM::VM(const std::vector<CompiledFn>& functions,
       SignalGraph& signals,
       PlatformBackend* backend,
       std::string appName,
       PluginManager* plugins,
       const PermissionMap* pluginPermissions)
    : functions_(functions),
      signals_(signals),
      backend_(backend),
      appName_(std::move(appName)),
      plugins_(plugins),
      pluginPermissions_(pluginPermissions) {}

const std::string& VM::currentPlugin() const {
    static const std::string kEmpty;
    return pluginCallStack_.empty() ? kEmpty : pluginCallStack_.back();
}

void VM::enforceRuntimePermission(const std::string& requiredPerm,
                                  const std::string& opName,
                                  bool requirePluginContext) const {
    const std::string& owner = currentPlugin();
    if (owner.empty()) {
        if (requirePluginContext) {
            throw std::runtime_error("SecurityError: " + opName +
                                     "() cannot be called outside a .tplug plugin");
        }
        return;
    }
    if (!pluginPermissions_) {
        throw std::runtime_error("SecurityError: plugin '" + owner +
                                 "' has no permission table for " + opName + "()");
    }
    auto it = pluginPermissions_->find(owner);
    if (it != pluginPermissions_->end()) {
        for (const auto& p : it->second) {
            if (p == requiredPerm) return;
        }
    }
    throw std::runtime_error("SecurityError: plugin '" + owner + "' lacks runtime permission '" +
                             requiredPerm + "' required by " + opName + "()");
}

Value VM::callBuiltin(int id, const std::vector<Value>& args) {
    switch (id) {
        case 0: // abs
            if (args[0].isInt()) return Value(std::llabs(args[0].asInt()));
            return Value(std::fabs(args[0].asFloat()));
        case 1: // round
            return Value(static_cast<int64_t>(std::llround(args[0].asFloat())));
        case 2: // min
            return (args[0].asFloat() < args[1].asFloat()) ? args[0] : args[1];
        case 3: // max
            return (args[0].asFloat() > args[1].asFloat()) ? args[0] : args[1];
        case 4: { // clamp
            double v = args[0].asFloat();
            double lo = args[1].asFloat();
            double hi = args[2].asFloat();
            return Value(std::clamp(v, lo, hi));
        }
        case 5: // len (String, List, or Map)
            if (args[0].isList()) {
                auto l = args[0].asList();
                return Value(static_cast<int64_t>(l ? l->size() : 0));
            }
            if (args[0].isMap()) {
                auto m = args[0].asMap();
                return Value(static_cast<int64_t>(m ? m->size() : 0));
            }
            return Value(static_cast<int64_t>(args[0].asString().size()));
        case 6: // toString
            return Value(args[0].toString());
        case 7: // toInt
            if (args[0].isString()) {
                try { return Value(static_cast<int64_t>(std::stoll(args[0].asString()))); }
                catch (...) { return Value(static_cast<int64_t>(0)); }
            }
            return Value(args[0].asInt());
        case 8: // toFloat
            if (args[0].isString()) {
                try { return Value(std::stod(args[0].asString())); }
                catch (...) { return Value(0.0); }
            }
            return Value(args[0].asFloat());
        case 9: { // print
            std::string s = args[0].toString();
            if (onPrint) onPrint(s);
            else std::cout << s << "\n";
            return Value(Null{});
        }
        case 10: { // push(list, value)
            if (args[0].isList()) {
                auto l = args[0].asList();
                if (l) l->push_back(args[1]);
            }
            return Value(Null{});
        }
        case 11: { // removeAt(list, index)
            if (args[0].isList()) {
                auto l = args[0].asList();
                int64_t idx = args[1].asInt();
                if (l && idx >= 0 && static_cast<size_t>(idx) < l->size()) {
                    l->erase(l->begin() + idx);
                }
            }
            return Value(Null{});
        }
        case 12: { // clear(list or map)
            if (args[0].isList()) {
                auto l = args[0].asList();
                if (l) l->clear();
            } else if (args[0].isMap()) {
                auto m = args[0].asMap();
                if (m) m->clear();
            }
            return Value(Null{});
        }
        case 13: // fsRead
            enforceRuntimePermission("fs.read", "fsRead");
            return Value(stdlib::fsRead(args[0].toString()));
        case 14: // fsWrite
            enforceRuntimePermission("fs.write", "fsWrite");
            return Value(stdlib::fsWrite(args[0].toString(), args[1].toString()));
        case 15: // fsExists
            enforceRuntimePermission("fs.read", "fsExists");
            return Value(stdlib::fsExists(args[0].toString()));
        case 16: // storageGet
            return Value(stdlib::storageGet(appName_, args[0].toString(), args[1].toString()));
        case 17: // storageSet
            stdlib::storageSet(appName_, args[0].toString(), args[1].toString());
            return Value(Null{});
        case 18: // clipboardCopy
            if (backend_) backend_->clipboardCopy(args[0].toString());
            return Value(Null{});
        case 19: // clipboardPaste
            return Value(backend_ ? backend_->clipboardPaste() : std::string());
        case 20: // openUrl
            if (backend_) backend_->openUrl(args[0].toString());
            return Value(Null{});
        case 21: // dialogOpenFile
            return Value(backend_ ? backend_->dialogOpenFile() : std::string());
        case 22: // dialogSaveFile
            return Value(backend_ ? backend_->dialogSaveFile() : std::string());
        case 23: { // httpFetch(url, targetStateName)
            enforceRuntimePermission("net.http", "httpFetch");
            if (backend_) {
                std::string url = args[0].toString();
                std::string targetState = args[1].toString();
                backend_->httpGetAsync(url, [this, targetState](std::string body) {
                    backend_->dispatchToMain([this, targetState, body = std::move(body)]() mutable {
                        signals_.setState(targetState, Value(std::move(body)));
                    });
                });
            }
            return Value(Null{});
        }
        case 24: // jsonGet
            return Value(stdlib::jsonGet(args[0].toString(), args[1].toString()));
        case 25: // bytesRead
            enforceRuntimePermission("fs.read", "bytesRead");
            return Value(stdlib::bytesRead(args[0].toString()));
        case 26: { // bytesLen
            auto b = args[0].asBytes();
            return Value(static_cast<int64_t>(b ? b->size() : 0));
        }
        case 27: { // bytesAt
            auto b = args[0].asBytes();
            int64_t idx = args[1].asInt();
            if (b && idx >= 0 && static_cast<size_t>(idx) < b->size()) {
                return Value(static_cast<int64_t>((*b)[idx]));
            }
            return Value(static_cast<int64_t>(-1));
        }
        case 28: // bytesSlice
            return Value(stdlib::bytesSlice(args[0].asBytes(), args[1].asInt(), args[2].asInt()));
        case 29: // bytesToString
            return Value(stdlib::bytesToString(args[0].asBytes()));
        case 30: // bytesToHex
            return Value(stdlib::bytesToHex(args[0].asBytes()));
        case 31: // strTrim
            return Value(stdlib::strTrim(args[0].toString()));
        case 32: { // strSplit
            auto parts = stdlib::strSplit(args[0].toString(), args[1].toString());
            auto list = std::make_shared<std::vector<Value>>();
            list->reserve(parts.size());
            for (auto& p : parts) list->emplace_back(std::move(p));
            return Value(std::move(list));
        }
        case 33: // strContains
            return Value(stdlib::strContains(args[0].toString(), args[1].toString()));
        case 34: // strStartsWith
            return Value(stdlib::strStartsWith(args[0].toString(), args[1].toString()));
        case 35: // strReplace
            return Value(stdlib::strReplace(args[0].toString(), args[1].toString(), args[2].toString()));
        case 36: // strCharAt
            return Value(stdlib::strCharAt(args[0].toString(), args[1].asInt()));
        case 37: // strSub
            return Value(stdlib::strSub(args[0].toString(), args[1].asInt(), args[2].asInt()));
        case 38: { // sysExec(cmd, argsList, timeoutMs, maxOutputBytes) -> ExecResult
            std::string cmd = args[0].toString();
            enforceRuntimePermission("process.exec:" + cmd, "sysExec", true);
            std::vector<std::string> cmdArgs;
            if (auto l = args[1].asList()) {
                for (const auto& item : *l) cmdArgs.push_back(item.toString());
            }
            int timeoutMs = static_cast<int>(args[2].asInt());
            size_t maxOut = static_cast<size_t>(args[3].asInt() > 0 ? args[3].asInt() : 1048576);
            auto er = stdlib::sysExec(cmd, cmdArgs, timeoutMs, maxOut);
            auto obj = std::make_shared<StructObject>();
            obj->typeName = "ExecResult";
            obj->fields["exitCode"] = Value(er.exitCode);
            obj->fields["stdout"] = Value(std::move(er.stdoutStr));
            obj->fields["stderr"] = Value(std::move(er.stderrStr));
            obj->fields["timedOut"] = Value(er.timedOut);
            return Value(std::move(obj));
        }
        case 39: // setTheme(name) -> Bool
            if (onSetTheme) return Value(onSetTheme(args[0].toString()));
            return Value(false);
        case 40: { // httpRequest(method, url, headers, body, targetState)
            enforceRuntimePermission("net.http", "httpRequest");
            if (backend_) {
                std::string method = args[0].toString();
                std::string url = args[1].toString();
                std::string headers = args[2].toString();
                std::string body = args[3].toString();
                std::string targetState = args[4].toString();
                backend_->httpRequestAsync(method, url, headers, body, [this, targetState](std::string resp) {
                    backend_->dispatchToMain([this, targetState, resp = std::move(resp)]() mutable {
                        signals_.setState(targetState, Value(std::move(resp)));
                    });
                });
            }
            return Value(Null{});
        }
        case 41: // jsonPath(json, path) -> String
            return Value(stdlib::jsonPath(args[0].toString(), args[1].toString()));
        case 42: // mapNew() -> Map<String, Any>
            return Value(std::make_shared<std::unordered_map<std::string, Value>>());
        case 43: { // hasKey(map, key) -> Bool
            auto m = args[0].asMap();
            return Value(m && m->count(args[1].toString()) > 0);
        }
        case 44: { // keys(map) -> List<String> (sorted)
            auto list = std::make_shared<std::vector<Value>>();
            if (auto m = args[0].asMap()) {
                std::vector<std::string> ks;
                ks.reserve(m->size());
                for (const auto& [k, _] : *m) ks.push_back(k);
                std::sort(ks.begin(), ks.end());
                for (auto& k : ks) list->emplace_back(std::move(k));
            }
            return Value(std::move(list));
        }
        case 45: { // removeKey(map, key) -> Void
            if (auto m = args[0].asMap()) m->erase(args[1].toString());
            return Value(Null{});
        }
        case 46: { // range(start, end) -> List<Int>
            int64_t a = args[0].asInt();
            int64_t b = args[1].asInt();
            auto list = std::make_shared<std::vector<Value>>();
            if (b > a) {
                list->reserve(static_cast<size_t>(b - a));
                for (int64_t i = a; i < b; ++i) list->emplace_back(i);
            }
            return Value(std::move(list));
        }
        case 47: { // Ok(val) -> Result<T>
            auto obj = std::make_shared<StructObject>();
            obj->typeName = "$Result";
            obj->fields["ok"] = Value(true);
            obj->fields["val"] = args[0];
            obj->fields["err"] = Value("");
            return Value(std::move(obj));
        }
        case 48: { // Err(msg) -> Result<Any>
            auto obj = std::make_shared<StructObject>();
            obj->typeName = "$Result";
            obj->fields["ok"] = Value(false);
            obj->fields["val"] = Value(Null{});
            obj->fields["err"] = Value(args[0].toString());
            return Value(std::move(obj));
        }
        case 49: { // isOk(r) -> Bool
            auto s = args[0].asStruct();
            return Value(s && s->typeName == "$Result" && s->fields["ok"].asBool());
        }
        case 50: { // isErr(r) -> Bool
            auto s = args[0].asStruct();
            return Value(!s || s->typeName != "$Result" || !s->fields["ok"].asBool());
        }
        case 51: { // unwrap(r) -> T
            auto s = args[0].asStruct();
            if (s && s->typeName == "$Result" && s->fields["ok"].asBool()) {
                return s->fields["val"];
            }
            return Value(Null{});
        }
        case 52: { // unwrapOr(r, fallback) -> T
            auto s = args[0].asStruct();
            if (s && s->typeName == "$Result" && s->fields["ok"].asBool()) {
                return s->fields["val"];
            }
            return args[1];
        }
        case 53: { // errMsg(r) -> String
            auto s = args[0].asStruct();
            if (s && s->typeName == "$Result") return s->fields["err"];
            return Value("");
        }
        case 54: { // __coalesce(a, b): unwraps Ok(v), or falls back to b on null / empty string / Err
            const Value& a = args[0];
            const Value& b = args[1];
            if (a.isNull()) return b;
            if (a.isString() && a.asString().empty()) return b;
            if (auto s = a.asStruct(); s && s->typeName == "$Result") {
                return s->fields["ok"].asBool() ? s->fields["val"] : b;
            }
            return a;
        }
        case 55: // eventName() -> String
            return Value(plugins_ ? plugins_->lastEventName() : std::string());
        case 56: // eventPayload() -> String
            return Value(plugins_ ? plugins_->lastEventPayload() : std::string());
        case 57: { // listJoin(list, sep) -> String
            std::string sep = args[1].toString();
            std::string out;
            if (auto l = args[0].asList()) {
                for (size_t i = 0; i < l->size(); ++i) {
                    if (i > 0) out += sep;
                    out += (*l)[i].toString();
                }
            }
            return Value(std::move(out));
        }
        case 58: { // listReverse(list) -> List<T>
            auto out = std::make_shared<std::vector<Value>>();
            if (auto l = args[0].asList()) {
                out->assign(l->rbegin(), l->rend());
            }
            return Value(std::move(out));
        }
        case 59: { // listSort(list) -> List<T>
            auto out = std::make_shared<std::vector<Value>>();
            if (auto l = args[0].asList()) {
                *out = *l;
                std::sort(out->begin(), out->end(), [](const Value& a, const Value& b) {
                    if ((a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat())) {
                        return a.asFloat() < b.asFloat();
                    }
                    return a.toString() < b.toString();
                });
            }
            return Value(std::move(out));
        }
        case 60: { // listContains(list, item) -> Bool
            if (auto l = args[0].asList()) {
                for (const auto& v : *l) {
                    if (v == args[1]) return Value(true);
                }
            }
            return Value(false);
        }
        case 61: { // listSlice(list, start, end) -> List<T>
            auto out = std::make_shared<std::vector<Value>>();
            if (auto l = args[0].asList()) {
                int64_t n = static_cast<int64_t>(l->size());
                int64_t a = std::max<int64_t>(0, std::min<int64_t>(n, args[1].asInt()));
                int64_t b = std::max<int64_t>(a, std::min<int64_t>(n, args[2].asInt()));
                for (int64_t i = a; i < b; ++i) out->push_back((*l)[static_cast<size_t>(i)]);
            }
            return Value(std::move(out));
        }
        case 62: { // strUpper(s) -> String
            std::string s = args[0].toString();
            for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return Value(std::move(s));
        }
        case 63: { // strLower(s) -> String
            std::string s = args[0].toString();
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return Value(std::move(s));
        }
        default:
            return Value(Null{});
    }
}

Value VM::execute(const Chunk& chunk,
                  const std::vector<Value>& args,
                  const ScopeMap* captured,
                  const std::string& ownerPlugin) {
    struct PluginScopeGuard {
        std::vector<std::string>& stack;
        PluginScopeGuard(std::vector<std::string>& s, const std::string& owner) : stack(s) {
            stack.push_back(owner);
        }
        ~PluginScopeGuard() { stack.pop_back(); }
    } scopeGuard(pluginCallStack_, ownerPlugin);

    std::vector<Value> stack;
    stack.reserve(64);
    for (const auto& a : args) stack.push_back(a);

    auto pop = [&]() -> Value {
        if (stack.empty()) throw std::runtime_error("VM stack underflow");
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };

    size_t ip = 0;
    const auto& code = chunk.code;
    while (ip < code.size()) {
        OpCode op = static_cast<OpCode>(code[ip++]);
        switch (op) {
            case OpCode::Const: {
                uint16_t idx = chunk.readU16(ip); ip += 2;
                stack.push_back(chunk.constants[idx]);
                break;
            }
            case OpCode::Null: stack.push_back(Value(Null{})); break;
            case OpCode::True: stack.push_back(Value(true)); break;
            case OpCode::False: stack.push_back(Value(false)); break;
            case OpCode::Pop: pop(); break;

            case OpCode::GetLocal: {
                uint16_t slot = chunk.readU16(ip); ip += 2;
                stack.push_back(stack[slot]);
                break;
            }
            case OpCode::SetLocal: {
                uint16_t slot = chunk.readU16(ip); ip += 2;
                stack[slot] = stack.back();
                break;
            }
            case OpCode::GetSignal: {
                uint16_t idx = chunk.readU16(ip); ip += 2;
                const std::string& name = chunk.constants[idx].asString();
                if (captured) {
                    auto it = captured->find(name);
                    if (it != captured->end()) {
                        stack.push_back(it->second);
                        break;
                    }
                }
                stack.push_back(signals_.get(name));
                break;
            }
            case OpCode::SetSignal: {
                uint16_t idx = chunk.readU16(ip); ip += 2;
                signals_.setState(chunk.constants[idx].asString(), stack.back());
                break;
            }
            case OpCode::NotifySignal: {
                uint16_t idx = chunk.readU16(ip); ip += 2;
                signals_.notifyChanged(chunk.constants[idx].asString());
                break;
            }

            case OpCode::MakeList: {
                uint16_t count = chunk.readU16(ip); ip += 2;
                auto list = std::make_shared<std::vector<Value>>(count);
                for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
                    (*list)[i] = pop();
                }
                stack.push_back(Value(std::move(list)));
                break;
            }
            case OpCode::MakeStruct: {
                uint16_t typeIdx = chunk.readU16(ip); ip += 2;
                uint16_t count = chunk.readU16(ip); ip += 2;
                auto obj = std::make_shared<StructObject>();
                obj->typeName = chunk.constants[typeIdx].asString();
                for (uint16_t i = 0; i < count; ++i) {
                    Value val = pop();
                    Value key = pop();
                    obj->fields[key.asString()] = std::move(val);
                }
                stack.push_back(Value(std::move(obj)));
                break;
            }
            case OpCode::GetIndex: {
                Value idxV = pop();
                Value objV = pop();
                if (objV.isList()) {
                    auto l = objV.asList();
                    int64_t idx = idxV.asInt();
                    if (l && idx >= 0 && static_cast<size_t>(idx) < l->size()) {
                        stack.push_back((*l)[idx]);
                        break;
                    }
                } else if (objV.isMap()) {
                    auto m = objV.asMap();
                    if (m) {
                        auto it = m->find(idxV.toString());
                        if (it != m->end()) {
                            stack.push_back(it->second);
                            break;
                        }
                    }
                }
                stack.push_back(Value(Null{}));
                break;
            }
            case OpCode::SetIndex: {
                Value val = pop();
                Value idxV = pop();
                Value objV = pop();
                if (objV.isList()) {
                    auto l = objV.asList();
                    int64_t idx = idxV.asInt();
                    if (l && idx >= 0 && static_cast<size_t>(idx) < l->size()) {
                        (*l)[idx] = std::move(val);
                    }
                } else if (objV.isMap()) {
                    auto m = objV.asMap();
                    if (m) {
                        (*m)[idxV.toString()] = std::move(val);
                    }
                }
                break;
            }
            case OpCode::GetField: {
                uint16_t fIdx = chunk.readU16(ip); ip += 2;
                Value objV = pop();
                if (objV.isStruct()) {
                    auto s = objV.asStruct();
                    auto it = s->fields.find(chunk.constants[fIdx].asString());
                    if (it != s->fields.end()) {
                        stack.push_back(it->second);
                        break;
                    }
                }
                stack.push_back(Value(Null{}));
                break;
            }
            case OpCode::SetField: {
                uint16_t fIdx = chunk.readU16(ip); ip += 2;
                Value val = pop();
                Value objV = pop();
                if (objV.isStruct()) {
                    auto s = objV.asStruct();
                    if (s) s->fields[chunk.constants[fIdx].asString()] = std::move(val);
                }
                break;
            }

            case OpCode::Add: {
                Value b = pop();
                Value a = pop();
                if (a.isString() || b.isString()) {
                    stack.push_back(Value(a.toString() + b.toString()));
                } else if (a.isInt() && b.isInt()) {
                    stack.push_back(Value(a.asInt() + b.asInt()));
                } else {
                    stack.push_back(Value(a.asFloat() + b.asFloat()));
                }
                break;
            }
            case OpCode::Sub: {
                Value b = pop();
                Value a = pop();
                if (a.isInt() && b.isInt()) stack.push_back(Value(a.asInt() - b.asInt()));
                else stack.push_back(Value(a.asFloat() - b.asFloat()));
                break;
            }
            case OpCode::Mul: {
                Value b = pop();
                Value a = pop();
                if (a.isInt() && b.isInt()) stack.push_back(Value(a.asInt() * b.asInt()));
                else stack.push_back(Value(a.asFloat() * b.asFloat()));
                break;
            }
            case OpCode::Div: {
                Value b = pop();
                Value a = pop();
                double d = b.asFloat();
                if (d == 0.0) throw std::runtime_error("Division by zero");
                if (a.isInt() && b.isInt()) stack.push_back(Value(a.asInt() / b.asInt()));
                else stack.push_back(Value(a.asFloat() / d));
                break;
            }
            case OpCode::Mod: {
                Value b = pop();
                Value a = pop();
                if (b.asInt() == 0) throw std::runtime_error("Modulo by zero");
                stack.push_back(Value(a.asInt() % b.asInt()));
                break;
            }
            case OpCode::Neg: {
                Value a = pop();
                if (a.isInt()) stack.push_back(Value(-a.asInt()));
                else stack.push_back(Value(-a.asFloat()));
                break;
            }
            case OpCode::Not: {
                Value a = pop();
                stack.push_back(Value(!a.asBool()));
                break;
            }
            case OpCode::Eq: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a == b));
                break;
            }
            case OpCode::Neq: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a != b));
                break;
            }
            case OpCode::Lt: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a.asFloat() < b.asFloat()));
                break;
            }
            case OpCode::Le: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a.asFloat() <= b.asFloat()));
                break;
            }
            case OpCode::Gt: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a.asFloat() > b.asFloat()));
                break;
            }
            case OpCode::Ge: {
                Value b = pop();
                Value a = pop();
                stack.push_back(Value(a.asFloat() >= b.asFloat()));
                break;
            }

            case OpCode::ToString: {
                Value a = pop();
                stack.push_back(Value(a.toString()));
                break;
            }
            case OpCode::Concat: {
                uint16_t n = chunk.readU16(ip); ip += 2;
                std::vector<std::string> parts(n);
                for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                    parts[i] = pop().toString();
                }
                std::string joined;
                for (const auto& p : parts) joined += p;
                stack.push_back(Value(std::move(joined)));
                break;
            }

            case OpCode::Jump: {
                uint16_t target = chunk.readU16(ip);
                ip = target;
                break;
            }
            case OpCode::JumpIfFalse: {
                uint16_t target = chunk.readU16(ip); ip += 2;
                if (!stack.back().asBool()) {
                    ip = target;
                }
                break;
            }

            case OpCode::Call: {
                uint16_t fnIdx = chunk.readU16(ip); ip += 2;
                uint8_t argc = code[ip++];
                std::vector<Value> callArgs(argc);
                for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                    callArgs[i] = pop();
                }
                const auto& targetFn = functions_[fnIdx];
                stack.push_back(execute(targetFn.chunk, callArgs, nullptr, targetFn.ownerPlugin));
                break;
            }
            case OpCode::CallBuiltin: {
                uint16_t bid = chunk.readU16(ip); ip += 2;
                uint8_t argc = code[ip++];
                std::vector<Value> callArgs(argc);
                for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                    callArgs[i] = pop();
                }
                stack.push_back(callBuiltin(static_cast<int>(bid), callArgs));
                break;
            }
            case OpCode::CallPlugin: {
                uint16_t nameIdx = chunk.readU16(ip); ip += 2;
                uint8_t argc = code[ip++];
                std::vector<Value> callArgs(argc);
                for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
                    callArgs[i] = pop();
                }
                if (plugins_) {
                    stack.push_back(plugins_->call(chunk.constants[nameIdx].asString(), callArgs));
                } else {
                    stack.push_back(Value(Null{}));
                }
                break;
            }
            case OpCode::Return:
                return stack.empty() ? Value(Null{}) : pop();
        }
    }
    return stack.empty() ? Value(Null{}) : stack.back();
}

} // namespace transom
