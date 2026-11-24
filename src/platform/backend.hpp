#pragma once

#include "common/value.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace transom {

using WidgetHandle = uint64_t;
constexpr WidgetHandle kInvalidWidget = 0;

struct WindowConfig {
    std::string title = "Transom";
    int width = 480;
    int height = 360;
    std::string bg = "#181a20";
};

// Runtime half of plugin-provided (native) widgets. Implemented by PluginManager;
// the platform backend calls it for any widget kind it does not build in.
class NativeWidgetHost {
public:
    virtual ~NativeWidgetHost() = default;
    virtual bool hasNativeWidget(const std::string& kind) const = 0;
    // Returns an opaque plugin instance (nullptr on failure). Main thread only.
    virtual void* createNativeWidget(const std::string& kind) = 0;
    virtual void setNativeWidgetProp(const std::string& kind, void* instance,
                                     const std::string& name, const Value& value) = 0;
    // Platform view owned by the instance: NSView* (macOS) / HWND (Windows).
    virtual void* nativeWidgetHandle(const std::string& kind, void* instance) = 0;
    virtual void destroyNativeWidget(const std::string& kind, void* instance) = 0;
    // Plugin -> app events: the sink runs the widget's trailing action block.
    virtual void setNativeWidgetEventSink(
        const std::string& kind, void* instance,
        std::function<void(const std::string& event, const Value& payload)> sink) = 0;
};

// Abstract native UI & OS platform backend.
// macOS implements this with AppKit/Cocoa; future platforms implement Win32/GTK.
class PlatformBackend {
public:
    virtual ~PlatformBackend() = default;

    // Window & Widget tree
    virtual void createWindow(const WindowConfig& cfg) = 0;
    virtual void setWindowBg(const std::string& hex) = 0;
    virtual void clearWindow() = 0;

    // Gives the backend access to plugin-provided widgets (may be null).
    virtual void setNativeWidgetHost(NativeWidgetHost* host) = 0;

    virtual WidgetHandle createWidget(const std::string& kind) = 0;
    virtual void setProp(WidgetHandle h, const std::string& name, const Value& value) = 0;
    virtual void addChild(WidgetHandle parent, WidgetHandle child) = 0;
    virtual void clearChildren(WidgetHandle parent) = 0;
    virtual void setRoot(WidgetHandle root) = 0;

    virtual void setOnClick(WidgetHandle h, std::function<void()> cb) = 0;
    virtual void setOnChange(WidgetHandle h, std::function<void(Value)> cb) = 0;

    // Timers & Main-Thread Async Dispatch
    virtual void addTimer(int intervalMs, std::function<void()> cb) = 0;
    virtual void dispatchToMain(std::function<void()> cb) = 0;

    // Native OS Services
    virtual void clipboardCopy(const std::string& text) = 0;
    virtual std::string clipboardPaste() = 0;
    virtual void openUrl(const std::string& url) = 0;
    virtual std::string dialogOpenFile() = 0;
    virtual std::string dialogSaveFile() = 0;
    virtual void httpGetAsync(const std::string& url,
                              std::function<void(std::string)> onDone) = 0;
    virtual void httpRequestAsync(const std::string& method,
                                  const std::string& url,
                                  const std::string& headers,
                                  const std::string& body,
                                  std::function<void(std::string)> onDone) = 0;

    // Runs the native event loop until the window closes.
    virtual void runEventLoop(std::function<void()> onTick = nullptr) = 0;
};

std::unique_ptr<PlatformBackend> createNativeBackend();

} // namespace transom
