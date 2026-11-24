#pragma once

#include "common/value.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transom {

// Fine-grained reactive signal graph:
//   - State cells hold mutable values.
//   - Derived cells hold a compute callback + list of upstream signal names.
//   - Bindings subscribe to any set of signal names and fire only when one
//     of their dependencies actually changes.
class SignalGraph {
public:
    using ComputeFn = std::function<Value()>;
    using ObserverFn = std::function<void()>;

    void defineState(const std::string& name, Value initial) {
        values_[name] = std::move(initial);
    }

    void defineDerived(const std::string& name,
                       std::vector<std::string> deps,
                       ComputeFn compute) {
        derivedOrder_.push_back(name);
        derivedCompute_[name] = std::move(compute);
        for (const auto& dep : deps) {
            dependents_[dep].push_back(name);
        }
        values_[name] = derivedCompute_[name]();
    }

    const Value& get(const std::string& name) const {
        static const Value kNull;
        auto it = values_.find(name);
        return (it != values_.end()) ? it->second : kNull;
    }

    bool has(const std::string& name) const {
        return values_.count(name) > 0;
    }

    // Subscribe an observer to a list of signal names.
    void subscribe(const std::vector<std::string>& signals, ObserverFn fn) {
        size_t id = observers_.size();
        observers_.push_back(std::move(fn));
        for (const auto& sig : signals) {
            signalObservers_[sig].push_back(id);
        }
    }

    // Mutate a state signal and propagate changes only to affected derived
    // signals and UI observers.
    bool setState(const std::string& name, Value newVal) {
        auto it = values_.find(name);
        if (it != values_.end() && it->second == newVal) {
            return false;
        }
        values_[name] = std::move(newVal);
        propagate(name);
        return true;
    }

    // Notify dependents after an in-place mutation of a List or Struct state.
    void notifyChanged(const std::string& name) {
        if (!has(name)) return;
        propagate(name);
    }

private:
    void propagate(const std::string& name) {
        std::unordered_set<std::string> dirty;
        dirty.insert(name);
        collectDownstream(name, dirty);

        // Recompute dirty deriveds in declaration (topological) order
        std::unordered_set<std::string> actuallyChanged;
        actuallyChanged.insert(name);
        for (const auto& dname : derivedOrder_) {
            if (!dirty.count(dname)) continue;
            Value next = derivedCompute_[dname]();
            if (values_[dname] != next) {
                values_[dname] = std::move(next);
                actuallyChanged.insert(dname);
            }
        }

        // Notify unique observers subscribed to any changed signal
        // Snapshot observer IDs first because a dynamic view rebuild may add observers.
        std::unordered_set<size_t> toNotify;
        for (const auto& changed : actuallyChanged) {
            auto oit = signalObservers_.find(changed);
            if (oit == signalObservers_.end()) continue;
            for (size_t id : oit->second) toNotify.insert(id);
        }
        std::vector<size_t> ordered(toNotify.begin(), toNotify.end());
        for (size_t id : ordered) {
            if (id < observers_.size() && observers_[id]) {
                observers_[id]();
            }
        }
    }

    void collectDownstream(const std::string& sig,
                           std::unordered_set<std::string>& dirty) const {
        auto it = dependents_.find(sig);
        if (it == dependents_.end()) return;
        for (const auto& dep : it->second) {
            if (dirty.insert(dep).second) {
                collectDownstream(dep, dirty);
            }
        }
    }

    std::unordered_map<std::string, Value> values_;
    std::vector<std::string> derivedOrder_;
    std::unordered_map<std::string, ComputeFn> derivedCompute_;
    std::unordered_map<std::string, std::vector<std::string>> dependents_;

    std::vector<ObserverFn> observers_;
    std::unordered_map<std::string, std::vector<size_t>> signalObservers_;
};

} // namespace transom
