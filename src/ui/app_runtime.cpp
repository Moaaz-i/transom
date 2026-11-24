#include "ui/app_runtime.hpp"
#include "stdlib/system_ops.hpp"

#include <sstream>

namespace transom {

AppRuntime::AppRuntime(PlatformBackend& backend, PluginManager* plugins)
    : backend_(backend), plugins_(plugins) {
    backend_.setNativeWidgetHost(plugins_);
}

AppRuntime::~AppRuntime() {
    if (vm_) {
        for (const auto& [pname, ch] : program_.pluginOnUnload) {
            vm_->execute(ch, {}, nullptr, pname);
        }
    }
}

void AppRuntime::mount(CompiledProgram program) {
    program_ = std::move(program);
    signals_ = SignalGraph{};
    styledWidgets_.clear();
    activeTheme_ = "default";

    vm_ = std::make_unique<VM>(program_.functions, signals_, &backend_,
                               program_.hasApp ? program_.app.title : "Transom",
                               plugins_, &program_.pluginPermissions);
    vm_->onSetTheme = [this](const std::string& name) {
        return this->setTheme(name);
    };

    // 0. Run plugin onLoad lifecycle hooks
    for (const auto& [pname, ch] : program_.pluginOnLoad) {
        vm_->execute(ch, {}, nullptr, pname);
    }

    if (!program_.hasApp) return;

    // 1. Initialize reactive states (with automatic load/save for `persist state`)
    for (const auto& st : program_.app.states) {
        Value initVal = vm_->execute(st.init.chunk);
        if (st.isPersistent) {
            std::string saved = stdlib::storageGet(program_.app.title, "persist:" + st.name, "");
            if (!saved.empty()) {
                try {
                    if (initVal.isInt()) initVal = Value(static_cast<int64_t>(std::stoll(saved)));
                    else if (initVal.isFloat()) initVal = Value(std::stod(saved));
                    else if (initVal.isBool()) initVal = Value(saved == "true" || saved == "1");
                    else if (initVal.isString()) initVal = Value(saved);
                } catch (...) {}
            }
        }
        signals_.defineState(st.name, std::move(initVal));
        if (st.isPersistent) {
            std::string appTitle = program_.app.title;
            std::string stateName = st.name;
            signals_.subscribe({stateName}, [this, appTitle, stateName]() {
                stdlib::storageSet(appTitle, "persist:" + stateName, signals_.get(stateName).toString());
            });
        }
    }

    // 2. Register derived signals
    for (const auto& dr : program_.app.deriveds) {
        const Chunk* chunkPtr = &dr.expr.chunk;
        signals_.defineDerived(dr.name, dr.expr.signalDeps, [this, chunkPtr]() {
            return vm_->execute(*chunkPtr);
        });
    }

    // 3. Configure native window (use $bg token if no inline bg prop)
    WindowConfig cfg;
    cfg.title = program_.app.title;
    std::string tokenBg = program_.styleSheet.resolveWindowBg(activeTheme_);
    if (!tokenBg.empty()) cfg.bg = tokenBg;

    for (const auto& wp : program_.app.windowProps) {
        Value v = vm_->execute(wp.expr.chunk);
        if (wp.name == "width") cfg.width = static_cast<int>(v.asInt());
        else if (wp.name == "height") cfg.height = static_cast<int>(v.asInt());
        else if (wp.name == "bg") cfg.bg = v.toString();
    }
    backend_.createWindow(cfg);

    // 4. Build native widget tree & attach reactive bindings
    ScopeMap emptyScope;
    WidgetHandle root = buildView(program_.app.rootView, emptyScope);
    backend_.setRoot(root);

    // 5. Register reactive timers (every(ms) { ... })
    for (const auto& tm : program_.app.timers) {
        int intervalMs = static_cast<int>(vm_->execute(tm.intervalMs.chunk).asInt());
        if (intervalMs < 10) intervalMs = 10;
        const Chunk* bodyPtr = &tm.body;
        backend_.addTimer(intervalMs, [this, bodyPtr]() {
            vm_->execute(*bodyPtr);
        });
    }
}

void AppRuntime::reload(CompiledProgram program) {
    if (vm_) {
        for (const auto& [pname, ch] : program_.pluginOnUnload) {
            vm_->execute(ch, {}, nullptr, pname);
        }
    }
    backend_.clearWindow();
    mount(std::move(program));
}

void AppRuntime::reloadStyle(CompiledStyleSheet newSheet) {
    program_.styleSheet = std::move(newSheet);
    if (!program_.styleSheet.hasTheme(activeTheme_)) {
        activeTheme_ = "default";
    }
    reconcileAllStyles();
}

bool AppRuntime::setTheme(const std::string& themeName) {
    if (!program_.styleSheet.hasTheme(themeName)) {
        return false; // Reject unknown theme without altering current theme
    }
    if (activeTheme_ == themeName) return true;
    activeTheme_ = themeName;
    reconcileAllStyles();
    return true;
}

void AppRuntime::applyResolvedStyles(StyledWidgetRecord& rec) {
    const auto& ss = program_.styleSheet;
    if (ss.empty()) return;

    // Deterministic 3-layer precedence:
    //   1. Widget Defaults  <  2. Class Styles  <  3. Inline Props (skipped here)
    std::unordered_map<std::string, Value> computed;

    auto wit = ss.widgetDefaults.find(rec.widgetKind);
    if (wit != ss.widgetDefaults.end()) {
        for (const auto& [pname, sval] : wit->second.normalProps) {
            if (!rec.inlineProps.count(pname)) {
                computed[pname] = ss.resolveValue(sval, activeTheme_);
            }
        }
    }

    for (const auto& cname : rec.classes) {
        auto cit = ss.classes.find(cname);
        if (cit == ss.classes.end()) continue;
        for (const auto& [pname, sval] : cit->second.normalProps) {
            if (!rec.inlineProps.count(pname)) {
                computed[pname] = ss.resolveValue(sval, activeTheme_);
            }
        }
    }

    // Incremental update: only call backend_.setProp when a value actually changed!
    for (const auto& [pname, val] : computed) {
        auto lit = rec.lastApplied.find(pname);
        if (lit == rec.lastApplied.end() || lit->second != val) {
            backend_.setProp(rec.handle, pname, val);
            rec.lastApplied[pname] = val;
        }
    }
}

void AppRuntime::reconcileAllStyles() {
    std::string winBg = program_.styleSheet.resolveWindowBg(activeTheme_);
    if (!winBg.empty()) {
        backend_.setWindowBg(winBg);
    }
    for (auto& rec : styledWidgets_) {
        applyResolvedStyles(rec);
    }
}

WidgetHandle AppRuntime::buildView(const CompiledView& node, const ScopeMap& captured) {
    if (node.kind == CompiledViewKind::If) {
        WidgetHandle slot = backend_.createWidget("VStack");
        backend_.setProp(slot, "gap", Value(8));

        const CompiledView* nodePtr = &node;
        auto renderBranch = [this, slot, nodePtr, captured]() {
            backend_.clearChildren(slot);
            bool cond = vm_->execute(nodePtr->condition.chunk, {}, &captured).asBool();
            const auto& branch = cond ? nodePtr->children : nodePtr->elseChildren;
            for (const auto& child : branch) {
                WidgetHandle ch = buildView(child, captured);
                backend_.addChild(slot, ch);
            }
        };

        renderBranch();
        if (!node.condition.signalDeps.empty()) {
            signals_.subscribe(node.condition.signalDeps, renderBranch);
        }
        return slot;
    }

    if (node.kind == CompiledViewKind::For) {
        WidgetHandle slot = backend_.createWidget("VStack");
        backend_.setProp(slot, "gap", Value(8));

        const CompiledView* nodePtr = &node;
        auto renderList = [this, slot, nodePtr, captured]() {
            backend_.clearChildren(slot);
            Value iterVal = vm_->execute(nodePtr->iterable.chunk, {}, &captured);
            auto list = iterVal.asList();
            if (!list) return;
            for (size_t i = 0; i < list->size(); ++i) {
                ScopeMap rowScope = captured;
                if (!nodePtr->indexVar.empty()) {
                    rowScope[nodePtr->indexVar] = Value(static_cast<int64_t>(i));
                }
                rowScope[nodePtr->itemVar] = (*list)[i];
                for (const auto& child : nodePtr->children) {
                    WidgetHandle ch = buildView(child, rowScope);
                    backend_.addChild(slot, ch);
                }
            }
        };

        renderList();
        if (!node.iterable.signalDeps.empty()) {
            signals_.subscribe(node.iterable.signalDeps, renderList);
        }
        return slot;
    }

    // Normal widget
    WidgetHandle h = backend_.createWidget(node.widget);

    // Prepare style record (collect class names & inline prop names first)
    StyledWidgetRecord rec;
    rec.handle = h;
    rec.widgetKind = node.widget;
    for (const auto& prop : node.props) {
        if (prop.name == "class") {
            Value v = vm_->execute(prop.expr.chunk, {}, &captured);
            std::istringstream iss(v.toString());
            std::string cname;
            while (iss >> cname) rec.classes.push_back(cname);
        } else {
            rec.inlineProps.insert(prop.name);
        }
    }

    // Apply layer 1 (Widget Defaults) & layer 2 (Class Styles) BEFORE inline props
    if (!program_.styleSheet.empty()) {
        applyResolvedStyles(rec);
        styledWidgets_.push_back(std::move(rec));
    }

    // Apply layer 3 (Inline Props — always highest precedence)
    for (const auto& prop : node.props) {
        if (prop.name == "class") continue;

        Value initial = vm_->execute(prop.expr.chunk, {}, &captured);
        backend_.setProp(h, prop.name, initial);

        if (!prop.expr.signalDeps.empty()) {
            const Chunk* exprChunk = &prop.expr.chunk;
            std::string propName = prop.name;
            signals_.subscribe(prop.expr.signalDeps, [this, h, propName, exprChunk, captured]() {
                Value updated = vm_->execute(*exprChunk, {}, &captured);
                backend_.setProp(h, propName, updated);
            });
        }

        if (!prop.bindState.empty()) {
            std::string stateName = prop.bindState;
            backend_.setOnChange(h, [this, stateName](Value v) {
                signals_.setState(stateName, std::move(v));
            });
        }
    }

    if (node.hasAction) {
        const Chunk* actionChunk = &node.actionChunk;
        backend_.setOnClick(h, [this, actionChunk, captured]() {
            vm_->execute(*actionChunk, {}, &captured);
        });
    }

    for (const auto& child : node.children) {
        WidgetHandle ch = buildView(child, captured);
        backend_.addChild(h, ch);
    }

    return h;
}

} // namespace transom
