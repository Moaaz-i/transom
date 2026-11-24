#include "common/source.hpp"
#include "frontend/lexer.hpp"
#include "frontend/module_loader.hpp"
#include "frontend/parser.hpp"
#include "platform/backend.hpp"
#include "plugin/plugin_manager.hpp"
#include "sema/type_checker.hpp"
#include "style/style_checker.hpp"
#include "style/style_parser.hpp"
#include "layout/layout_engine.hpp"
#include "ui/app_runtime.hpp"
#include "vm/compiler.hpp"
#include "vm/serializer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace transom;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& msg) {
    if (cond) {
        g_passed++;
    } else {
        g_failed++;
        std::cerr << "FAIL: " << msg << "\n";
    }
}

// Headless mock backend to test full declarative UI trees, button clicks,
// input/slider two-way bindings, and fine-grained reactive updates without a GUI!
class MockBackend final : public PlatformBackend {
public:
    struct Node {
        std::string kind;
        std::unordered_map<std::string, Value> props;
        std::vector<WidgetHandle> children;
        std::function<void()> onClick;
        std::function<void(Value)> onChange;
        int updateCount = 0;
        void* nativeInst = nullptr;
    };

    WindowConfig window;
    WidgetHandle root = kInvalidWidget;
    WidgetHandle nextId = 1;
    int createWidgetCalls = 0;
    std::unordered_map<WidgetHandle, Node> nodes;

    void createWindow(const WindowConfig& cfg) override { window = cfg; }
    void setWindowBg(const std::string& hex) override { window.bg = hex; }
    void clearWindow() override {
        for (auto& [id, n] : nodes) destroyNative(n);
        nodes.clear();
        root = kInvalidWidget;
    }

    NativeWidgetHost* nativeHost = nullptr;
    void setNativeWidgetHost(NativeWidgetHost* host) override { nativeHost = host; }

    WidgetHandle createWidget(const std::string& kind) override {
        createWidgetCalls++;
        WidgetHandle h = nextId++;
        nodes[h] = Node{kind, {}, {}, nullptr, nullptr, 0};
        if (nativeHost && nativeHost->hasNativeWidget(kind)) {
            nodes[h].nativeInst = nativeHost->createNativeWidget(kind);
        }
        return h;
    }

    void setProp(WidgetHandle h, const std::string& name, const Value& value) override {
        nodes[h].props[name] = value;
        nodes[h].updateCount++;
        if (nodes[h].nativeInst && nativeHost) {
            nativeHost->setNativeWidgetProp(nodes[h].kind, nodes[h].nativeInst, name, value);
        }
    }

    void destroyNative(Node& n) {
        if (n.nativeInst && nativeHost) nativeHost->destroyNativeWidget(n.kind, n.nativeInst);
        n.nativeInst = nullptr;
    }

    void addChild(WidgetHandle parent, WidgetHandle child) override {
        nodes[parent].children.push_back(child);
    }

    void removeSubtree(WidgetHandle h) {
        auto it = nodes.find(h);
        if (it == nodes.end()) return;
        auto kids = it->second.children;
        for (WidgetHandle c : kids) removeSubtree(c);
        destroyNative(it->second);
        nodes.erase(h);
    }

    void clearChildren(WidgetHandle parent) override {
        auto it = nodes.find(parent);
        if (it == nodes.end()) return;
        auto kids = it->second.children;
        it->second.children.clear();
        for (WidgetHandle c : kids) removeSubtree(c);
    }

    void setRoot(WidgetHandle r) override { root = r; }
    void setOnClick(WidgetHandle h, std::function<void()> cb) override {
        if (nodes[h].nativeInst && nativeHost) {
            nativeHost->setNativeWidgetEventSink(nodes[h].kind, nodes[h].nativeInst,
                [cb](const std::string&, const Value&) { if (cb) cb(); });
            return;
        }
        nodes[h].onClick = std::move(cb);
    }
    void setOnChange(WidgetHandle h, std::function<void(Value)> cb) override { nodes[h].onChange = std::move(cb); }

    std::vector<std::function<void()>> timers;
    std::string clipboard;
    std::string lastOpenedUrl;
    std::string mockHttpResponse = "{\"status\":\"ok\",\"version\":\"2.0\"}";

    void addTimer(int, std::function<void()> cb) override { timers.push_back(std::move(cb)); }
    void dispatchToMain(std::function<void()> cb) override { if (cb) cb(); }
    void clipboardCopy(const std::string& text) override { clipboard = text; }
    std::string clipboardPaste() override { return clipboard; }
    void openUrl(const std::string& url) override { lastOpenedUrl = url; }
    std::string dialogOpenFile() override { return "/tmp/mock.txt"; }
    std::string dialogSaveFile() override { return "/tmp/save.txt"; }
    std::string lastHttpMethod;
    std::string lastHttpHeaders;
    std::string lastHttpBody;
    void httpGetAsync(const std::string&, std::function<void(std::string)> onDone) override {
        if (onDone) onDone(mockHttpResponse);
    }
    void httpRequestAsync(const std::string& method,
                          const std::string&,
                          const std::string& headers,
                          const std::string& body,
                          std::function<void(std::string)> onDone) override {
        lastHttpMethod = method;
        lastHttpHeaders = headers;
        lastHttpBody = body;
        if (onDone) onDone("{\"choices\":[{\"message\":{\"content\":\"Hello from REST API!\"}}]}");
    }
    void runEventLoop(std::function<void()>) override {}

    WidgetHandle findByKindAndProp(const std::string& kind,
                                   const std::string& prop,
                                   const std::string& val) const {
        for (const auto& [id, n] : nodes) {
            if (n.kind != kind) continue;
            auto it = n.props.find(prop);
            if (it != n.props.end() && it->second.toString() == val) return id;
        }
        return kInvalidWidget;
    }

    WidgetHandle findFirst(const std::string& kind) const {
        for (const auto& [id, n] : nodes) {
            if (n.kind == kind) return id;
        }
        return kInvalidWidget;
    }
};

CompiledProgram compileSource(const std::string& src, DiagnosticEngine& diag) {
    Lexer lex(src, diag);
    auto toks = lex.tokenize();
    Parser parser(std::move(toks), diag);
    auto prog = parser.parseProgram();
    TypeChecker tc(diag);
    tc.check(prog);
    if (diag.hasErrors()) return {};
    Compiler comp;
    return comp.compile(prog);
}

void testLexerInterpolation() {
    DiagnosticEngine diag;
    Lexer lex("state msg = \"Count: {count + 1}!\"\n", diag);
    auto toks = lex.tokenize();
    check(!diag.hasErrors(), "lexer interpolation: no errors");
    bool sawPart = false, sawStart = false, sawEnd = false, sawStrEnd = false;
    for (const auto& t : toks) {
        if (t.kind == TokenKind::StringPart) sawPart = true;
        if (t.kind == TokenKind::InterpStart) sawStart = true;
        if (t.kind == TokenKind::InterpEnd) sawEnd = true;
        if (t.kind == TokenKind::StringEnd) sawStrEnd = true;
    }
    check(sawPart && sawStart && sawEnd && sawStrEnd,
          "lexer interpolation emits StringPart/InterpStart/InterpEnd/StringEnd");
}

void testStaticTypeErrors() {
    // 1. Wrong prop type
    {
        DiagnosticEngine diag;
        compileSource(R"(
app "Bad" (width: "not-a-number") {
    view Text("hello")
}
)", diag);
        check(diag.hasErrors(), "typechecker catches wrong window prop type");
    }

    // 2. Unknown widget property
    {
        DiagnosticEngine diag;
        compileSource(R"(
app "Bad" {
    view Text("hello", fontSize: 20)
}
)", diag);
        check(diag.hasErrors(), "typechecker catches unknown widget property");
    }

    // 3. Mutating derived signal
    {
        DiagnosticEngine diag;
        compileSource(R"(
app "Bad" {
    state x = 1
    derived doubled = x * 2
    view Button("Hack") {
        doubled = 10
    }
}
)", diag);
        check(diag.hasErrors(), "typechecker rejects assignment to derived signal");
    }

    // 4. Binding Input to non-state or wrong type
    {
        DiagnosticEngine diag;
        compileSource(R"(
app "Bad" {
    state count = 0
    view Input(bind: count)
}
)", diag);
        check(diag.hasErrors(), "typechecker rejects Input bound to Int state");
    }
}

void testReactiveCounterEndToEnd() {
    const std::string src = R"(
fn step(n: Int) -> Int {
    return n + 1
}

app "Counter" (width: 400, height: 280, bg: "#101218") {
    state count = 0
    derived title = "Clicks: {count}"
    derived mood = if count > 0 { "Positive" } else if count < 0 { "Negative" } else { "Zero" }

    view VStack(gap: 12, padding: 20) {
        Text(title, size: 24, weight: "bold")
        Text(mood, size: 14)
        HStack(gap: 8) {
            Button("-") { count -= 1 }
            Button("+") { count = step(count) }
            Button("Reset") { count = 0 }
        }
    }
}
)";

    DiagnosticEngine diag;
    auto compiled = compileSource(src, diag);
    check(!diag.hasErrors(), "counter app compiles cleanly: " + diag.formatAll());

    MockBackend backend;
    AppRuntime runtime(backend);
    runtime.mount(std::move(compiled));

    check(backend.window.title == "Counter", "window title set");
    check(backend.window.width == 400 && backend.window.height == 280, "window size set");

    WidgetHandle titleText = backend.findByKindAndProp("Text", "text", "Clicks: 0");
    WidgetHandle moodText = backend.findByKindAndProp("Text", "text", "Zero");
    WidgetHandle plusBtn = backend.findByKindAndProp("Button", "label", "+");
    WidgetHandle minusBtn = backend.findByKindAndProp("Button", "label", "-");
    WidgetHandle resetBtn = backend.findByKindAndProp("Button", "label", "Reset");

    check(titleText != kInvalidWidget, "initial title text rendered");
    check(moodText != kInvalidWidget, "initial mood text rendered");
    check(plusBtn != kInvalidWidget && minusBtn != kInvalidWidget && resetBtn != kInvalidWidget,
          "all buttons created");

    // Click '+' twice -> reactive signals update title & mood automatically
    backend.nodes[plusBtn].onClick();
    check(backend.nodes[titleText].props["text"].toString() == "Clicks: 1",
          "reactive update after first '+' click");
    check(backend.nodes[moodText].props["text"].toString() == "Positive",
          "derived 'if' expression updates mood to Positive");

    int moodUpdatesBefore = backend.nodes[moodText].updateCount;
    backend.nodes[plusBtn].onClick();
    check(backend.nodes[titleText].props["text"].toString() == "Clicks: 2",
          "reactive update after second '+' click");
    // Fine-grained check: mood stayed "Positive", so moodText should NOT be re-updated!
    check(backend.nodes[moodText].updateCount == moodUpdatesBefore,
          "fine-grained reactivity skips unchanged derived UI updates");

    // Reset
    backend.nodes[resetBtn].onClick();
    check(backend.nodes[titleText].props["text"].toString() == "Clicks: 0", "reset sets count to 0");
    check(backend.nodes[moodText].props["text"].toString() == "Zero", "reset updates mood to Zero");

    // Click '-'
    backend.nodes[minusBtn].onClick();
    check(backend.nodes[titleText].props["text"].toString() == "Clicks: -1", "'-' decrements count");
    check(backend.nodes[moodText].props["text"].toString() == "Negative", "mood updates to Negative");
}

void testComponentsAndTwoWayBinding() {
    const std::string src = R"(
component Badge(label: String) {
    Card(padding: 12, radius: 8) {
        Text(label, size: 16, weight: "semibold")
    }
}

app "Greeter" {
    state name = ""
    state celsius = 20.0
    derived greeting = if len(name) == 0 { "Enter your name" } else { "Hello, {name}!" }
    derived fahrenheit = round(celsius * 9.0 / 5.0 + 32.0)

    view VStack(gap: 10) {
        Badge(label: greeting)
        Input(bind: name, placeholder: "Your name")
        Text("{fahrenheit} F")
        Slider(bind: celsius, min: 0.0, max: 100.0)
    }
}
)";

    DiagnosticEngine diag;
    auto compiled = compileSource(src, diag);
    check(!diag.hasErrors(), "components & two-way binding compiles cleanly: " + diag.formatAll());

    MockBackend backend;
    AppRuntime runtime(backend);
    runtime.mount(std::move(compiled));

    WidgetHandle greetingText = backend.findByKindAndProp("Text", "text", "Enter your name");
    WidgetHandle tempText = backend.findByKindAndProp("Text", "text", "68 F");
    WidgetHandle input = backend.findFirst("Input");
    WidgetHandle slider = backend.findFirst("Slider");

    check(greetingText != kInvalidWidget, "component expanded & rendered initial greeting");
    check(tempText != kInvalidWidget, "initial derived fahrenheit computed (20C -> 68F)");
    check(input != kInvalidWidget && slider != kInvalidWidget, "Input and Slider created");

    // Simulate user typing into Input
    backend.nodes[input].onChange(Value("Moaaz"));
    check(backend.nodes[greetingText].props["text"].toString() == "Hello, Moaaz!",
          "Input two-way binding updates component Text reactively");

    // Simulate dragging Slider to 100C
    backend.nodes[slider].onChange(Value(100.0));
    check(backend.nodes[tempText].props["text"].toString() == "212 F",
          "Slider two-way binding updates derived fahrenheit (100C -> 212F)");
}

void testDynamicTodoApp() {
    const std::string src = R"(
struct Task {
    title: String
    done: Bool
}

app "Todo" {
    state draft = ""
    state tasks: List<Task> = [
        Task(title: "Alpha", done: false)
    ]
    derived total = len(tasks)

    view VStack(gap: 8) {
        Text("Count: {total}")
        Input(bind: draft, placeholder: "Add")
        Button("Add") {
            push(tasks, Task(title: draft, done: false))
            draft = ""
        }
        if total == 0 {
            Text("Empty")
        } else {
            for (i, task) in tasks {
                HStack(gap: 6) {
                    Button(if task.done { "DONE" } else { "TODO" }) {
                        tasks[i].done = !tasks[i].done
                    }
                    Text(task.title)
                    Button("Del") {
                        removeAt(tasks, i)
                    }
                }
            }
        }
    }
}
)";

    DiagnosticEngine diag;
    auto compiled = compileSource(src, diag);
    check(!diag.hasErrors(), "todo app compiles cleanly: " + diag.formatAll());

    MockBackend backend;
    AppRuntime runtime(backend);
    runtime.mount(std::move(compiled));

    check(backend.findByKindAndProp("Text", "text", "Count: 1") != kInvalidWidget,
          "initial task count is 1");
    check(backend.findByKindAndProp("Text", "text", "Alpha") != kInvalidWidget,
          "initial task Alpha rendered by for view");
    check(backend.findByKindAndProp("Text", "text", "Empty") == kInvalidWidget,
          "Empty branch not shown when tasks non-empty");

    // Toggle Alpha's done field -> button label flips from TODO to DONE
    WidgetHandle toggleBtn = backend.findByKindAndProp("Button", "label", "TODO");
    check(toggleBtn != kInvalidWidget, "initial TODO toggle button exists");
    backend.nodes[toggleBtn].onClick();
    check(backend.findByKindAndProp("Button", "label", "DONE") != kInvalidWidget,
          "struct field mutation triggers reactive for-view update to DONE");

    // Add a new task "Beta"
    WidgetHandle input = backend.findFirst("Input");
    WidgetHandle addBtn = backend.findByKindAndProp("Button", "label", "Add");
    backend.nodes[input].onChange(Value("Beta"));
    backend.nodes[addBtn].onClick();
    check(backend.findByKindAndProp("Text", "text", "Count: 2") != kInvalidWidget,
          "push() updates derived len(tasks) to 2");
    check(backend.findByKindAndProp("Text", "text", "Beta") != kInvalidWidget,
          "new task Beta rendered in for view");

    // Delete both tasks -> if total == 0 branch appears!
    WidgetHandle del1 = backend.findByKindAndProp("Button", "label", "Del");
    backend.nodes[del1].onClick();
    check(backend.findByKindAndProp("Text", "text", "Count: 1") != kInvalidWidget,
          "removeAt() drops count to 1");
    WidgetHandle del2 = backend.findByKindAndProp("Button", "label", "Del");
    backend.nodes[del2].onClick();
    check(backend.findByKindAndProp("Text", "text", "Count: 0") != kInvalidWidget,
          "second removeAt() drops count to 0");
    check(backend.findByKindAndProp("Text", "text", "Empty") != kInvalidWidget,
          "if view dynamically switches to Empty branch");
}

void testModuleImportsAndRichDiagnostics() {
    // 1. Rich diagnostics show source line & caret
    {
        const std::string badSrc = "app \"Bad\" (width: \"oops\") {\n    view Text(\"hi\")\n}\n";
        DiagnosticEngine diag;
        compileSource(badSrc, diag);
        std::string formatted = diag.formatAll("bad.transom", badSrc);
        check(formatted.find("1 | app \"Bad\" (width: \"oops\")") != std::string::npos,
              "rich diagnostic includes offending source line");
        check(formatted.find("^") != std::string::npos,
              "rich diagnostic includes caret pointer");
    }

    // 2. Multi-file import resolution & circular import detection
    namespace fs = std::filesystem;
    fs::path tmpDir = fs::temp_directory_path() / "transom_mod_test";
    fs::create_directories(tmpDir);

    {
        std::ofstream(tmpDir / "helper.transom") << R"(
struct Note { text: String }
fn formatNote(n: Note) -> String { return "Note: {n.text}" }
component NoteView(label: String) { Text(label) }
)";
        std::ofstream(tmpDir / "main.transom") << R"(
import "./helper.transom"
app "ModApp" {
    state n = Note(text: "Hello Modules")
    derived msg = formatNote(n)
    view VStack { NoteView(label: msg) }
}
)";
        DiagnosticEngine diag;
        auto loaded = loadProgramWithImports((tmpDir / "main.transom").string(), diag);
        check(loaded.has_value() && !diag.hasErrors(),
              "multi-file import loads without errors: " + diag.formatAll());
        if (loaded) {
            TypeChecker tc(diag);
            tc.check(loaded->program);
            check(!diag.hasErrors(), "imported struct/fn/component pass typecheck: " + diag.formatAll());
            Compiler comp;
            auto cp = comp.compile(loaded->program);
            MockBackend backend;
            AppRuntime rt(backend);
            rt.mount(std::move(cp));
            check(backend.findByKindAndProp("Text", "text", "Note: Hello Modules") != kInvalidWidget,
                  "imported component & fn render properly");
        }
    }

    // Circular import check
    {
        std::ofstream(tmpDir / "a.transom") << "import \"./b.transom\"\n";
        std::ofstream(tmpDir / "b.transom") << "import \"./a.transom\"\n";
        DiagnosticEngine diag;
        auto loaded = loadProgramWithImports((tmpDir / "a.transom").string(), diag);
        check(!loaded.has_value() && diag.hasErrors(), "circular import is caught cleanly");
    }

    fs::remove_all(tmpDir);
}

void testSystemApisTimersAndUiPrimitives() {
    const std::string src = R"(
app "SystemStudio" (width: 500, height: 420) {
    state ticks = 0
    state darkMode = true
    state theme = "Ocean"
    state apiJson = ""
    derived apiVer = jsonGet(apiJson, "version")

    every(1000) {
        ticks += 1
    }

    view ScrollView(gap: 10, width: 460, height: 380) {
        Text("Ticks: {ticks}")
        Toggle("Dark mode", bind: darkMode)
        Select(options: ["Ocean", "Sunset", "Forest"], bind: theme)
        Progress(value: ticks, max: 10.0)
        Divider()
        Canvas(width: 200, height: 80, bg: "#111111", draw: "rect 10 10 40 40 #38bdf8")
        Text("API: {apiVer}")
        Button("Fetch & Copy") {
            httpFetch("https://example.com/api", "apiJson")
            clipboardCopy(theme)
        }
    }
}
)";

    DiagnosticEngine diag;
    auto compiled = compileSource(src, diag);
    check(!diag.hasErrors(), "system APIs + UI primitives compile cleanly: " + diag.formatAll());

    MockBackend backend;
    AppRuntime runtime(backend);
    runtime.mount(std::move(compiled));

    check(backend.timers.size() == 1, "every(1000) timer registered on backend");
    backend.timers[0]();
    backend.timers[0]();
    check(backend.findByKindAndProp("Text", "text", "Ticks: 2") != kInvalidWidget,
          "timer callback updates reactive state & UI");

    WidgetHandle toggle = backend.findFirst("Toggle");
    WidgetHandle select = backend.findFirst("Select");
    WidgetHandle canvas = backend.findFirst("Canvas");
    WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Fetch & Copy");
    check(toggle != kInvalidWidget && select != kInvalidWidget && canvas != kInvalidWidget,
          "Toggle, Select, and Canvas primitives mounted");

    backend.nodes[select].onChange(Value("Forest"));
    backend.nodes[btn].onClick();
    check(backend.clipboard == "Forest", "clipboardCopy receives reactive Select state");
    check(backend.findByKindAndProp("Text", "text", "API: 2.0") != kInvalidWidget,
          "httpFetch + dispatchToMain + jsonGet updates derived UI reactively");
}

void testBytecodeSerializationRoundTrip() {
    const std::string src = R"(
app "SerializedApp" (width: 360, height: 240) {
    state n = 5
    derived label = "Value: {n}"
    view VStack(gap: 8) {
        Text(label)
        Button("Inc") { n += 1 }
    }
}
)";
    DiagnosticEngine diag;
    auto compiled = compileSource(src, diag);
    check(!diag.hasErrors(), "serialization test source compiles cleanly");

    auto tbcPath = (std::filesystem::temp_directory_path() / "transom_test.tbc").string();
    check(saveBytecodeFile(compiled, tbcPath), "saveBytecodeFile writes .tbc");

    auto loaded = loadBytecodeFile(tbcPath);
    check(loaded.has_value() && loaded->hasApp, "loadBytecodeFile restores CompiledProgram");

    if (loaded) {
        MockBackend backend;
        AppRuntime rt(backend);
        rt.mount(std::move(*loaded));
        check(backend.window.title == "SerializedApp", "deserialized window title matches");
        WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Inc");
        check(btn != kInvalidWidget, "deserialized button exists");
        backend.nodes[btn].onClick();
        check(backend.findByKindAndProp("Text", "text", "Value: 6") != kInvalidWidget,
              "deserialized bytecode executes reactive update identically");
    }
    std::filesystem::remove(tbcPath);
}

void testNativePluginEndToEnd() {
    namespace fs = std::filesystem;
    if (!fs::exists("tests/fixtures/plugins/demo/libdemo.dylib") &&
        !fs::exists("tests/fixtures/plugins/demo/libdemo.so")) {
        std::cout << "  (skipped native plugin test: run 'make fixtures')\n";
        return;
    }

    const std::string src = R"(
use "demo"

app "NativeDemo" {
    state total = 0
    state greeting = ""
    state level = 0.0
    state ticks = 0
    view VStack {
        Text("Total: {total}")
        Text(greeting)
        Meter(0.5, label: "cpu") { ticks += 1 }
        Button("Go") {
            total = demo.add(40, 2)
            greeting = demo.greet("Transom")
            level = demo.lastMeterValue()
        }
    }
}
)";

    DiagnosticEngine diag;
    PluginManager plugins;
    Lexer lex(src, diag);
    Parser parser(lex.tokenize(), diag);
    auto prog = parser.parseProgram();
    check(!diag.hasErrors(), "native plugin app parses: " + diag.formatAll());

    // ---- Phase 1: compile time. Manifest only, NO native code loaded. ----
    for (const auto& u : prog.uses) plugins.loadPlugin(u.plugin, "tests/fixtures", u.span, diag);
    check(!diag.hasErrors(), "native manifest loads: " + diag.formatAll());
    check(!plugins.hasNativeWidget("Meter"), "compile phase does not load/bind native code");
    check(plugins.widgets().find("Meter") != nullptr, "widget schema registered from manifest");
    check(WidgetSchemaRegistry::builtins().find("Meter") == nullptr,
          "core built-in registry stays free of plugin widgets");

    TypeChecker tc(diag, &plugins);
    tc.check(prog);
    check(!diag.hasErrors(), "native fn + widget schema pass static typecheck: " + diag.formatAll());

    {   // Schema-level errors are caught at compile time without native code.
        DiagnosticEngine d;
        Lexer l2("use \"demo\"\napp \"B\" {\n view VStack {\n Meter(bogus: 1)\n }\n}\n", d);
        Parser p2(l2.tokenize(), d);
        auto bad = p2.parseProgram();
        TypeChecker t2(d, &plugins);
        t2.check(bad);
        check(d.hasErrors(), "typechecker rejects unknown prop on plugin widget");

        DiagnosticEngine d2;
        Lexer l3("use \"demo\"\napp \"B\" {\n state x = 0\n view Button(\"b\") {\n x = demo.add(\"a\", 2)\n }\n}\n", d2);
        Parser p3(l3.tokenize(), d2);
        auto bad2 = p3.parseProgram();
        TypeChecker t3(d2, &plugins);
        t3.check(bad2);
        check(d2.hasErrors(), "typechecker rejects wrong arg type for native plugin fn");
    }

    {   // Core knows no PDF: widget is unknown unless a plugin declares it.
        DiagnosticEngine d;
        Lexer l4("app \"B\" {\n view VStack {\n PdfView(src: \"x.pdf\")\n }\n}\n", d);
        Parser p4(l4.tokenize(), d);
        auto pr = p4.parseProgram();
        TypeChecker t4(d);
        t4.check(pr);
        check(d.hasErrors(), "core alone rejects unknown domain widget (no PDF in core)");
    }

    // ---- Phase 2: runtime. Trust policy first. ----
    {
        DiagnosticEngine d;
        plugins.setTrustPolicy({}, false);
        check(!plugins.bindRuntime(d) && d.formatAll().find("not trusted") != std::string::npos,
              "unpinned native plugin is refused (not trusted)");
    }
    auto entries = plugins.nativeLockEntries();
    check(entries.size() == 1, "native library hash available for locking");
    if (entries.size() != 1) return;
    {
        DiagnosticEngine d;
        plugins.setTrustPolicy({{"native:demo", std::string(64, '0')}}, false);
        check(!plugins.bindRuntime(d) && d.formatAll().find("hash mismatch") != std::string::npos,
              "tampered native library hash is refused");
    }
    {
        DiagnosticEngine d;
        plugins.setTrustPolicy({{"native:demo", entries[0].sha256}}, false);
        check(plugins.bindRuntime(d), "pinned native plugin binds: " + d.formatAll());
    }
    check(plugins.hasNativeWidget("Meter"), "runtime half of widget bound after trust check");

    Compiler comp;
    comp.setWidgetRegistry(&plugins.widgets());
    auto compiled = comp.compile(prog);
    MockBackend backend;
    AppRuntime rt(backend, &plugins);
    rt.mount(std::move(compiled));

    WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Go");
    check(btn != kInvalidWidget, "native demo button mounted");
    backend.nodes[btn].onClick();
    check(rt.signals().get("total").asInt() == 42, "native fn demo.add(40, 2) == 42");
    check(backend.findByKindAndProp("Text", "text", "Hello, Transom!") != kInvalidWidget,
          "native fn returns host-managed string");
    check(rt.signals().get("level").asFloat() > 0.49 && rt.signals().get("level").asFloat() < 0.51,
          "native widget received positional prop value=0.5 through vtable");
    check(plugins.call("demo.lastMeterLabel", {}).toString() == "cpu",
          "native widget received string prop (copied, call-scoped pointer)");
    plugins.call("demo.fireTick", {});
    plugins.call("demo.fireTick", {});
    check(rt.signals().get("ticks").asInt() == 2,
          "native widget event (emit_widget_event) runs the trailing action block");
    backend.clearWindow();
    check(plugins.call("demo.destroyedCount", {}).asInt() >= 1,
          "native widget instance destroyed through vtable");
}

void testTplugOfficialModulesAndSecurity() {
    namespace fs = std::filesystem;
    fs::path tmpDir = fs::temp_directory_path() / "transom_tplug_test";
    fs::create_directories(tmpDir);

    // 1. Test real RFC-4180 CSV .tplug module (quoted commas, escaped quotes, multiline)
    {
        fs::path appFile = tmpDir / "csv_app.transom";
        std::ofstream(appFile) << R"(
use "csv"

app "CsvTest" {
    state rowCount = 0
    state firstField = ""
    state secondField = ""

    view VStack {
        csv.CsvBadge(rows: rowCount)
        Text(firstField)
        Text(secondField)
        Button("Parse") {
            let line = "\"Hello, World\",\"He said \"\"Hi\"\"\",42"
            let cols = csv.parseRow(line)
            firstField = cols[0]
            secondField = cols[1]
            rowCount = csv.countRows("a,b\n\"line1\nline2\",c\n3,4")
        }
    }
}
)";
        DiagnosticEngine diag;
        auto loaded = loadProgramWithImports(appFile.string(), diag);
        check(loaded.has_value() && !diag.hasErrors(), "csv .tplug loads cleanly: " + diag.formatAll());
        if (loaded) {
            TypeChecker tc(diag);
            tc.check(loaded->program);
            check(!diag.hasErrors(), "csv .tplug passes typecheck: " + diag.formatAll());
            Compiler comp;
            auto cp = comp.compile(loaded->program);
            MockBackend backend;
            AppRuntime rt(backend);
            rt.mount(std::move(cp));
            WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Parse");
            backend.nodes[btn].onClick();
            check(backend.findByKindAndProp("Text", "text", "Hello, World") != kInvalidWidget,
                  "CSV .tplug parses quoted field with embedded comma");
            check(backend.findByKindAndProp("Text", "text", "He said \"Hi\"") != kInvalidWidget,
                  "CSV .tplug unescapes doubled quotes");
            check(backend.findByKindAndProp("Text", "text", "CSV • 3 rows") != kInvalidWidget,
                  "CSV .tplug counts rows with multiline quoted fields & renders exported component");
        }
    }

    // 2. Generic file-signature .tplug module (Bytes type, corrupt rejection, lockfile)
    {
        fs::path goodFile = tmpDir / "good.bin";
        fs::path badFile = tmpDir / "corrupt.bin";
        std::ofstream(goodFile, std::ios::binary) << "SIG-1.4\npayload\n";
        std::ofstream(badFile, std::ios::binary) << "NOT A SIGNED FILE";
        fs::create_directories(tmpDir / "plugins" / "filesig");
        fs::copy_file("tests/fixtures/modules/filesig/plugin.tplug",
                      tmpDir / "plugins" / "filesig" / "plugin.tplug",
                      fs::copy_options::overwrite_existing);

        fs::path appFile = tmpDir / "filesig_app.transom";
        std::ofstream(appFile) << "use \"filesig\"\napp \"FileSig\" {\n"
            << "    state validGood = false\n"
            << "    state validBad = true\n"
            << "    state size = 0\n"
            << "    view VStack {\n"
            << "        filesig.SigBadge(size: size)\n"
            << "        Button(\"Check\") {\n"
            << "            validGood = filesig.hasSig(\"" << goodFile.string() << "\", \"SIG-\")\n"
            << "            validBad = filesig.hasSig(\"" << badFile.string() << "\", \"SIG-\")\n"
            << "            size = filesig.byteCount(\"" << goodFile.string() << "\")\n"
            << "        }\n"
            << "    }\n}\n";

        DiagnosticEngine diag;
        auto loaded = loadProgramWithImports(appFile.string(), diag);
        check(loaded.has_value() && !diag.hasErrors(), "filesig .tplug loads cleanly: " + diag.formatAll());
        if (loaded) {
            TypeChecker tc(diag);
            tc.check(loaded->program);
            check(!diag.hasErrors(), "filesig .tplug passes typecheck: " + diag.formatAll());
            Compiler comp;
            auto cp = comp.compile(loaded->program);
            MockBackend backend;
            AppRuntime rt(backend);
            rt.mount(std::move(cp));
            WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Check");
            backend.nodes[btn].onClick();
            check(rt.signals().get("validGood").asBool() == true, "filesig .tplug validates magic bytes");
            check(rt.signals().get("validBad").asBool() == false, "filesig .tplug rejects non-matching file");
            check(rt.signals().get("size").asInt() >= 1, "filesig .tplug returns byteCount >= 1");

            // Test lockfile write & tamper detection
            fs::path lockFile = tmpDir / "transom.lock";
            check(writeLockfile(lockFile.string(), loaded->lockedPlugins), "writeLockfile succeeds");
            DiagnosticEngine lockDiag;
            check(verifyLockfile(lockFile.string(), loaded->lockedPlugins, lockDiag),
                  "verifyLockfile passes for unmodified plugin");
            auto tampered = loaded->lockedPlugins;
            tampered[0].sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
            check(!verifyLockfile(lockFile.string(), tampered, lockDiag),
                  "verifyLockfile catches tampered plugin SHA-256 hash");
            auto hashes = readLockedHashes(lockFile.string());
            check(hashes.count("filesig") == 1, "readLockedHashes exposes pinned plugin hashes");
        }
    }

    // 3. Level-1 Compile-Time Security: missing permission, private fn, direct sysExec in app
    {
        // Missing process.exec:echo permission
        DiagnosticEngine d1;
        compileSource(R"(
plugin "untrusted" {
    version: "1.0.0"
    permissions: ["fs.read"]
    export fn run() -> Int {
        let r = sysExec("echo", ["hi"], 1000, 1024)
        return r.exitCode
    }
}
)", d1);
        check(d1.hasErrors(), "compile-time security blocks sysExec without process.exec:<cmd>");

        // Consumer app calling sysExec directly
        DiagnosticEngine d2;
        compileSource(R"(
app "DirectExec" {
    view Button("Hack") {
        let r = sysExec("echo", ["hi"], 1000, 1024)
    }
}
)", d2);
        check(d2.hasErrors(), "compile-time security blocks direct sysExec in consumer app");

        // Private plugin function cannot be called by consumer app
        DiagnosticEngine d3;
        compileSource(R"(
plugin "secret" {
    version: "1.0.0"
    permissions: []
    fn hidden() -> Int { return 42 }
    export fn pubFn() -> Int { return hidden() }
}
app "Consumer" {
    state x = secret.hidden()
    view Text("{x}")
}
)", d3);
        check(d3.hasErrors(), "typechecker blocks calling private non-exported plugin fn");
    }

    // 4. Level-2 Runtime Security: tampered bytecode without runtime permission throws SecurityError!
    {
        DiagnosticEngine diag;
        auto cp = compileSource(R"(
plugin "runner" {
    version: "1.0.0"
    permissions: ["process.exec:echo"]
    export fn ping() -> String {
        let r = sysExec("echo", ["ok"], 2000, 1024)
        return strTrim(r.stdout)
    }
}
app "RunnerApp" {
    state out = ""
    view Button("Run") { out = runner.ping() }
}
)", diag);
        check(!diag.hasErrors(), "authorized runner plugin compiles cleanly: " + diag.formatAll());

        // Strip runtime permissions AFTER compilation to simulate tampered bytecode!
        cp.pluginPermissions["runner"].clear();
        MockBackend backend;
        AppRuntime rt(backend);
        rt.mount(std::move(cp));
        WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Run");
        bool caughtSecurity = false;
        try {
            backend.nodes[btn].onClick();
        } catch (const std::runtime_error& e) {
            if (std::string(e.what()).find("SecurityError") != std::string::npos) {
                caughtSecurity = true;
            }
        }
        check(caughtSecurity, "runtime VM security enforces capability even if bytecode permissions are stripped");
    }

    fs::remove_all(tmpDir);
}

void testTstyleDesignSystem() {
    namespace fs = std::filesystem;

    // 1. Undefined token -> Compile Error
    {
        DiagnosticEngine d;
        StyleParser sp("tokens { bg: \"#111111\" } style a { color: $missing }", d);
        auto ast = sp.parse();
        StyleChecker sc(d);
        sc.checkAndCompile(ast);
        check(d.hasErrors(), "tstyle: undefined $token triggers compile error");
    }

    // 2. Wrong property type -> Compile Error
    {
        DiagnosticEngine d;
        StyleParser sp("style a { radius: \"not-an-int\" }", d);
        auto ast = sp.parse();
        StyleChecker sc(d);
        sc.checkAndCompile(ast);
        check(d.hasErrors(), "tstyle: wrong property type triggers compile error");
    }

    // 3. Circular inheritance -> Compile Error
    {
        DiagnosticEngine d;
        StyleParser sp("style a : b { padding: 8 } style b : a { gap: 4 }", d);
        auto ast = sp.parse();
        StyleChecker sc(d);
        sc.checkAndCompile(ast);
        check(d.hasErrors(), "tstyle: circular inheritance is rejected at compile time");
    }

    // 4. Theme adding undeclared token -> Compile Error
    {
        DiagnosticEngine d;
        StyleParser sp("tokens { bg: \"#000000\" } theme \"light\" { brandNew: \"#ffffff\" }", d);
        auto ast = sp.parse();
        StyleChecker sc(d);
        sc.checkAndCompile(ast);
        check(d.hasErrors(), "tstyle: theme cannot declare new token absent from base tokens");
    }

    // 5. Full end-to-end .tstyle + precedence + inheritance + incremental setTheme + Hot Reload + .tbc
    fs::path tmpDir = fs::temp_directory_path() / "transom_tstyle_test";
    fs::create_directories(tmpDir);

    std::ofstream(tmpDir / "app.tstyle") << R"(
tokens {
    bg: "#101218"
    surface: "#1a1e2a"
    textCol: "#f8fafc"
    padBase: 12dp
}
theme "light" {
    bg: "#f8fafc"
    surface: "#ffffff"
    textCol: "#0f172a"
}
Card {
    bg: $surface
    padding: $padBase
    radius: 8
}
style baseCard : Card {
    radius: 14
}
style featuredCard : baseCard {
    borderWidth: 2
}
style title {
    size: 20pt
    color: $textCol
}
)";

    // 5a. Unknown class -> Compile Error
    {
        std::ofstream(tmpDir / "bad_class.transom") << R"(
style "./app.tstyle"
app "BadClass" { view Text("hi", class: "doesNotExist") }
)";
        DiagnosticEngine d;
        auto loaded = loadProgramWithImports((tmpDir / "bad_class.transom").string(), d);
        check(loaded.has_value(), "bad_class loads stylesheet");
        if (loaded) {
            TypeChecker tc(d);
            tc.check(loaded->program);
            check(d.hasErrors(), "tstyle: unknown class name triggers compile error");
        }
    }

    // 5b. Precedence, incremental theme switching, input preservation, style Hot Reload, .tbc
    {
        std::ofstream(tmpDir / "main.transom") << R"(
style "./app.tstyle"
app "Themed" {
    state draft = "keep me"
    state unknownOk = true
    view VStack {
        Text("Title", class: "title")
        Card(class: "featuredCard", padding: 30) {
            Input(bind: draft, placeholder: "x")
        }
        Button("ToLight") { setTheme("light") }
        Button("ToUnknown") { unknownOk = setTheme("nonexistent") }
    }
}
)";
        DiagnosticEngine d;
        auto loaded = loadProgramWithImports((tmpDir / "main.transom").string(), d);
        check(loaded.has_value() && !d.hasErrors(), "themed app loads cleanly: " + d.formatAll());
        if (loaded) {
            TypeChecker tc(d);
            tc.check(loaded->program);
            check(!d.hasErrors(), "themed app passes typecheck: " + d.formatAll());

            Compiler comp;
            auto cp = comp.compile(loaded->program);

            // Verify .tbc round-trip preserves CompiledStyleSheet
            auto tbcPath = (tmpDir / "themed.tbc").string();
            check(saveBytecodeFile(cp, tbcPath), "tstyle: saveBytecodeFile embeds styleSheet");
            auto restored = loadBytecodeFile(tbcPath);
            check(restored.has_value() && restored->styleSheet.version == kStyleSheetVersion &&
                      restored->styleSheet.hasTheme("light"),
                  "tstyle: loadBytecodeFile restores versioned CompiledStyleSheet");

            MockBackend backend;
            AppRuntime rt(backend);
            rt.mount(std::move(*restored));

            WidgetHandle titleH = backend.findByKindAndProp("Text", "text", "Title");
            WidgetHandle cardH = backend.findFirst("Card");
            WidgetHandle inputH = backend.findFirst("Input");
            WidgetHandle lightBtn = backend.findByKindAndProp("Button", "label", "ToLight");
            WidgetHandle unknownBtn = backend.findByKindAndProp("Button", "label", "ToUnknown");

            // Verify 3-layer precedence: Inline (padding=30) > Class+Inheritance (radius=14, borderWidth=2) > Widget Default (bg=#1a1e2a)
            check(backend.window.bg == "#101218", "tstyle: initial window bg uses default $bg token");
            check(backend.nodes[cardH].props["padding"].asInt() == 30, "tstyle: inline prop overrides widget default");
            check(backend.nodes[cardH].props["radius"].asInt() == 14, "tstyle: inherited class overrides widget default");
            check(backend.nodes[cardH].props["borderWidth"].asInt() == 2, "tstyle: child class prop applied");
            check(backend.nodes[cardH].props["bg"].toString() == "#1a1e2a", "tstyle: widget default token applied");
            check(backend.nodes[titleH].props["color"].toString() == "#f8fafc", "tstyle: class token applied");

            // Mutate Input text & record native widget creation count before switching theme
            backend.nodes[inputH].onChange(Value("user typed text"));
            int widgetsBeforeTheme = backend.createWidgetCalls;

            // Unknown theme returns false & does NOT change current theme
            backend.nodes[unknownBtn].onClick();
            check(rt.signals().get("unknownOk").asBool() == false && rt.activeTheme() == "default",
                  "tstyle: setTheme('nonexistent') returns false & preserves active theme");

            // Switch to light theme -> incremental update ONLY, zero recreated views, input state preserved!
            backend.nodes[lightBtn].onClick();
            check(backend.createWidgetCalls == widgetsBeforeTheme,
                  "tstyle: setTheme('light') does NOT recreate native views");
            check(backend.window.bg == "#f8fafc", "tstyle: window bg updated to light theme");
            check(backend.nodes[cardH].props["bg"].toString() == "#ffffff", "tstyle: card bg updated incrementally");
            check(backend.nodes[titleH].props["color"].toString() == "#0f172a", "tstyle: text color updated incrementally");
            check(rt.signals().get("draft").toString() == "user typed text",
                  "tstyle: setTheme preserves Input state");

            // Hot-reload stylesheet without losing state or recreating views!
            DiagnosticEngine sd;
            StyleParser sp2("tokens { bg: \"#222222\" surface: \"#333333\" textCol: \"#eeeeee\" padBase: 12 } Card { bg: $surface } style baseCard : Card { radius: 20 } style featuredCard : baseCard { borderWidth: 3 } style title { size: 24 color: $textCol }", sd);
            StyleChecker sc2(sd);
            auto newSheet = sc2.checkAndCompile(sp2.parse());
            rt.reloadStyle(std::move(newSheet));
            check(backend.createWidgetCalls == widgetsBeforeTheme,
                  "tstyle: Hot Reload style updates in-place without recreating views");
            check(backend.nodes[cardH].props["radius"].asInt() == 20 &&
                      rt.signals().get("draft").toString() == "user typed text",
                  "tstyle: Hot Reload applies new styles while preserving app state");
        }
    }

    fs::remove_all(tmpDir);
}

void testRestApiJsonPath() {
    const std::string src = R"(
app "CoreFreezeTest" {
    state apiResp = ""
    derived reply = jsonPath(apiResp, "choices.0.message.content")
    view VStack {
        Canvas(width: 300, height: 200, interactive: true, strokeColor: "#3b5bdb", strokeWidth: 3.0)
        Text(reply)
        Button("Post") {
            httpRequest("POST", "https://api.example.com/v1/chat", "Authorization: Bearer secret\nContent-Type: application/json", "\{\"hi\":1\}", "apiResp")
        }
    }
}
)";
    DiagnosticEngine d;
    auto cp = compileSource(src, d);
    check(!d.hasErrors(), "httpRequest + jsonPath + interactive Canvas compile cleanly: " + d.formatAll());

    MockBackend backend;
    AppRuntime rt(backend);
    rt.mount(std::move(cp));

    WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Post");
    backend.nodes[btn].onClick();

    check(backend.lastHttpMethod == "POST", "httpRequest sends POST method");
    check(backend.lastHttpHeaders.find("Bearer secret") != std::string::npos, "httpRequest sends custom headers");
    check(backend.findByKindAndProp("Text", "text", "Hello from REST API!") != kInvalidWidget,
          "jsonPath extracts nested array/object value reactively into UI");
}

} // namespace


void testPortableLayoutEngineGolden() {
    using namespace transom::layout;

    // 1. Test multi-viewport non-overlapping & bounds containment (360x640, 800x600, 1240x800, 1920x1080)
    const std::pair<float, float> viewports[] = {
        {360.0f, 640.0f}, {800.0f, 600.0f}, {1240.0f, 800.0f}, {1920.0f, 1080.0f}
    };
    for (auto [vw, vh] : viewports) {
        LayoutNode root{1, LayoutKind::VStack};
        root.style.gap = 8.0f;
        root.style.padding = 12.0f;
        root.style.align = Align::Stretch;

        LayoutNode header{2, LayoutKind::HStack};
        header.style.height = 48.0f;
        header.style.gap = 8.0f;
        LayoutNode title{3, LayoutKind::Leaf};
        title.style.intrinsicWidth = 120.0f;
        title.style.intrinsicHeight = 24.0f;
        LayoutNode sp{4, LayoutKind::Spacer};
        LayoutNode btn{5, LayoutKind::Leaf};
        btn.style.intrinsicWidth = 90.0f;
        btn.style.intrinsicHeight = 32.0f;
        header.children.push_back(title);
        header.children.push_back(sp);
        header.children.push_back(btn);

        LayoutNode body{6, LayoutKind::Grid};
        body.style.columns = 2;
        body.style.gap = 10.0f;
        body.style.grow = 1.0f;
        for (uint64_t i = 0; i < 4; ++i) {
            LayoutNode card{10 + i, LayoutKind::Card};
            card.style.intrinsicWidth = 100.0f;
            card.style.intrinsicHeight = 80.0f;
            body.children.push_back(card);
        }

        root.children.push_back(header);
        root.children.push_back(body);
        computeLayout(root, vw, vh);

        // Verify zero overlap between header and body, and containment within viewport
        check(root.frame.width <= vw + 0.5f && root.frame.height <= vh + 0.5f,
              "layout root fits within viewport");
        check(root.children[0].frame.bottom() <= root.children[1].frame.y + 0.5f,
              "VStack children do not overlap vertically");
        // Verify HStack Spacer pushes button to the right without overlapping title
        const auto& hKids = root.children[0].children;
        check(hKids[0].frame.right() <= hKids[2].frame.x + 0.5f,
              "HStack Spacer separates title and button without overlap");
        // Verify 2-column Grid places col 0 left of col 1 and row 0 above row 1
        const auto& gKids = root.children[1].children;
        check(gKids[0].frame.right() <= gKids[1].frame.x + 0.5f,
              "Grid column 0 does not overlap column 1");
        check(gKids[0].frame.bottom() <= gKids[2].frame.y + 0.5f,
              "Grid row 0 does not overlap row 1");
    }

    // 2. RTL mirroring test on HStack and Grid
    {
        LayoutNode row{1, LayoutKind::HStack};
        row.style.rtl = true;
        row.style.gap = 10.0f;
        LayoutNode a{2, LayoutKind::Leaf}; a.style.width = 60.0f; a.style.height = 30.0f;
        LayoutNode b{3, LayoutKind::Leaf}; b.style.width = 80.0f; b.style.height = 30.0f;
        row.children = {a, b};
        computeLayout(row, 400.0f, 100.0f);
        check(row.children[0].frame.x > row.children[1].frame.x,
              "RTL HStack mirrors first child to the right of second child");
    }

    // 3. ZStack & Grid & Modal compile cleanly through TypeChecker
    {
        DiagnosticEngine d;
        auto cp = compileSource(R"(
app "Layout3Test" {
    state showModal = false
    view ZStack(align: "center") {
        Grid(columns: 2, gap: 12, rtl: true) {
            Card(padding: 12) { Text("A") }
            Card(padding: 12) { Text("B") }
        }
        Modal(open: showModal) {
            Card(padding: 20) {
                Button("Close") { showModal = false }
            }
        }
    }
}
)", d);
        check(!d.hasErrors(), "Grid + ZStack + Modal + rtl compile cleanly: " + d.formatAll());
        MockBackend mb;
        AppRuntime rt(mb);
        rt.mount(std::move(cp));
        check(mb.findFirst("Grid") != kInvalidWidget, "Grid widget mounted");
        check(mb.findFirst("ZStack") != kInvalidWidget, "ZStack widget mounted");
        check(mb.findFirst("Modal") != kInvalidWidget, "Modal widget mounted");
    }
}


void testEnumAndMatch() {
    const std::string okSrc = R"(
enum Status { Idle, Loading, Done }

app "EnumDemo" {
    state s: Status = Status.Idle
    state label = "start"
    view VStack {
        Text(label)
        Button("Step") {
            match s {
                Status.Idle {
                    s = Status.Loading
                    label = "loading"
                }
                Loading {
                    s = Status.Done
                    label = "done"
                }
                Status.Done {
                    label = "finished"
                }
            }
        }
    }
}
)";
    DiagnosticEngine d;
    auto cp = compileSource(okSrc, d);
    check(!d.hasErrors(), "enum + match compile cleanly: " + d.formatAll());
    MockBackend backend;
    AppRuntime rt(backend);
    rt.mount(std::move(cp));
    WidgetHandle btn = backend.findByKindAndProp("Button", "label", "Step");
    backend.nodes[btn].onClick();
    check(backend.findByKindAndProp("Text", "text", "loading") != kInvalidWidget, "match arm Status.Idle runs");
    backend.nodes[btn].onClick();
    check(backend.findByKindAndProp("Text", "text", "done") != kInvalidWidget, "bare-variant arm Loading runs");
    backend.nodes[btn].onClick();
    check(backend.findByKindAndProp("Text", "text", "finished") != kInvalidWidget, "match arm Status.Done runs");

    auto expectError = [&](const std::string& src, const std::string& needle, const std::string& what) {
        DiagnosticEngine e;
        compileSource(src, e);
        check(e.hasErrors() && e.formatAll().find(needle) != std::string::npos,
              what + " (got: " + e.formatAll() + ")");
    };
    expectError(R"(
enum Mode { A, B }
app "X" {
    state m = Mode.C
    view VStack { Text("x") }
}
)", "has no variant 'C'", "unknown enum variant is a compile error");
    expectError(R"(
enum Mode { A, B }
app "X" {
    state m = Mode.A
    view Button("b") {
        match m {
            Mode.A { m = Mode.B }
        }
    }
}
)", "Non-exhaustive match", "non-exhaustive match is a compile error");
    expectError(R"(
app "X" {
    state n = 1
    view Button("b") {
        match n {
            else { n = 2 }
        }
    }
}
)", "match requires an enum value", "match on non-enum is rejected");
    {
        DiagnosticEngine e;
        compileSource(R"(
enum Mode { A, B }
app "X" {
    state m = Mode.A
    view Button("b") {
        match m {
            Mode.A { m = Mode.B }
            else { m = Mode.A }
        }
    }
}
)", e);
        check(!e.hasErrors(), "else arm satisfies exhaustiveness: " + e.formatAll());
    }
    expectError(R"(
enum Mode { A, B }
app "X" {
    state m: Mode = "A"
    view VStack { Text("x") }
}
)", "Mode", "enum is not assignable from String");
}

void testLayoutSpacerAndFill() {
    using namespace transom::layout;
    LayoutNode root; root.id = 1; root.kind = LayoutKind::VStack;
    root.style.align = Align::Center; root.style.padding = 0;
    LayoutNode row; row.id = 2; row.kind = LayoutKind::HStack; row.style.fillCross = true;
    LayoutNode title; title.id = 3; title.style.intrinsicWidth = 100; title.style.intrinsicHeight = 20;
    LayoutNode sp; sp.id = 4; sp.kind = LayoutKind::Spacer;
    LayoutNode btn; btn.id = 5; btn.style.intrinsicWidth = 80; btn.style.intrinsicHeight = 30;
    row.children = {title, sp, btn};
    root.children = {row};
    computeLayout(root, 600, 400);
    const auto& r = root.children[0];
    check(r.frame.width > 599.0f, "fillCross HStack stretches to the VStack width");
    check(r.children[2].frame.right() > 599.0f, "Spacer pushes trailing button to the far edge");

    // ScrollView must not shrink its content
    LayoutNode sv; sv.id = 1; sv.kind = LayoutKind::ScrollView; sv.style.noShrink = true;
    sv.style.height = 100;
    for (uint64_t i = 0; i < 5; ++i) {
        LayoutNode c; c.id = 10 + i; c.style.intrinsicWidth = 50; c.style.intrinsicHeight = 40;
        sv.children.push_back(c);
    }
    computeLayout(sv, 300, 100);
    check(sv.children[4].frame.bottom() > 150.0f && sv.children[0].frame.height > 39.0f,
          "ScrollView content keeps natural height (scrolls instead of squashing)");
}


void testCoreLanguageCompleteness() {
    const std::string src = R"(
const BASE_BONUS = 10
enum Role { Guest, Admin, Editor }

fn parseScore(raw: String) -> Result<Int> {
    if len(raw) == 0 {
        return Err("empty score")
    }
    return Ok(toInt(raw) + BASE_BONUS)
}

app "LangComplete" {
    state scores: Map<String, Int> = mapNew()
    state role: Role = Role.Admin
    state total = 0
    state summary = ""

    derived roleBadge = match role {
        Role.Guest => "Guest User"
        Role.Admin => "Administrator"
        else => "Editor"
    }

    view VStack {
        Text(roleBadge)
        Text("Total: {total}")
        Text(summary)
        Button("RunAll") {
            // 1. Map indexing, hasKey, keys, removeKey, len
            scores["alice"] = 30
            scores["bob"] = 20
            scores["temp"] = 999
            if hasKey(scores, "temp") {
                removeKey(scores, "temp")
            }

            // 2. For loop over range() with continue & break
            let acc = 0
            for n in range(0, 10) {
                if n == 2 { continue }
                if n == 5 { break }
                acc += n
            }
            // n=0,1,3,4 -> acc = 8

            // 3. For loop with (idx, key) over keys(scores)
            for (idx, k) in keys(scores) {
                acc += scores[k] + idx
            }
            // alice(30+0) + bob(20+1) + 8 = 59

            // 4. Result<T> + ?? coalescing + |> pipe operator
            let good = "31" |> parseScore
            let bad = "" |> parseScore
            let v1 = good ?? 0
            let v2 = bad ?? 100
            // v1 = 31 + 10 = 41, v2 = 100 -> total = 59 + 41 + 100 = 200
            total = acc + v1 + v2

            // 5. Pipe operator chaining + string match
            let cleaned = "  hello transom  " |> strTrim |> strReplace("transom", "3.0")
            match cleaned {
                "hello 3.0" => summary = "OK:{len(scores)}:{errMsg(bad)}"
                else => summary = "FAIL"
            }
        }
    }
}
)";

    DiagnosticEngine d;
    auto cp = compileSource(src, d);
    check(!d.hasErrors(), "complete language suite (const, Map, Result, ??, |>, for/range/break/continue, MatchExpr) compiles cleanly: " + d.formatAll());
    if (d.hasErrors()) return;

    MockBackend backend;
    AppRuntime rt(backend);
    rt.mount(std::move(cp));

    check(backend.findByKindAndProp("Text", "text", "Administrator") != kInvalidWidget,
          "MatchExpr derived signal evaluates Role.Admin => Administrator");

    WidgetHandle btn = backend.findByKindAndProp("Button", "label", "RunAll");
    check(btn != kInvalidWidget, "RunAll button mounted");
    backend.nodes[btn].onClick();

    check(rt.signals().get("total").asInt() == 200,
          "Map + for/range/break/continue + Result<Int> + ?? + |> compute total == 200 (got " +
              rt.signals().get("total").toString() + ")");
    check(backend.findByKindAndProp("Text", "text", "OK:2:empty score") != kInvalidWidget,
          "Pipe chain + primitive match + errMsg(bad) produce 'OK:2:empty score'");

    // Verify compile-time guards for break outside loop and const reassignment
    {
        DiagnosticEngine e1;
        compileSource(R"(app "B" { view Button("x") { break } })", e1);
        check(e1.hasErrors(), "break outside loop is rejected at compile time");

        DiagnosticEngine e2;
        compileSource(R"(const K = 5
app "B" { view Button("x") { K = 6 } })", e2);
        check(e2.hasErrors(), "assigning to const is rejected at compile time");
    }

    // Verify malformed/recoverable syntax & small errors never hang and report all issues in one pass
    {
        const char* brokenSrc = R"(
app "Game" (width: 100, height: 100) {
    fn Rectangle(x: Num, y: Num, width: Num, height: Num, redius: Num) {
        return "Rectangle: " + x.toString() + y.toString() + width.toString() + height.toString() + redius.toString()
    }
    component Rec2(x: Num, y: Num) {
        return Rectangle(x: x, y: y, width: 20, height: 20, radius: 10)
    }

    fn main() {
        let s = Rec2(x: 10, y: 10)
        print(s)
    }
}
)";
        DiagnosticEngine ed;
        compileSource(brokenSrc, ed);
        std::string all = ed.formatAll("broken.transom", brokenSrc);
        check(ed.hasErrors(), "broken app with small errors is caught immediately");
        check(all.find("'component' must be declared at top level") != std::string::npos,
              "reports component inside app");
        check(all.find("must contain a UI widget tree (not 'return')") != std::string::npos,
              "reports return inside component");
        check(all.find("Unknown type 'Num' (did you mean 'Int' or 'Float'?)") != std::string::npos,
              "reports helpful suggestion for Num type");
        check(all.find("Method '.toString()' is not supported on values; use 'toString(") != std::string::npos,
              "reports helpful suggestion for .toString() method call");
        check(all.find("Function has no return type annotation (defaults to Void), but returns String") != std::string::npos,
              "reports missing -> String return type annotation on fn");
        check(all.find("UI component/widget 'Rec2' can only be used inside a 'view' tree") != std::string::npos,
              "reports calling UI component as value expression inside fn");
        check(all.find("App 'Game' is missing a 'view' declaration") != std::string::npos,
              "reports missing view declaration in app");
    }
    // Verify Component Slots, Default Parameters, persist state, and collection/string pipeline helpers
    {
        const char* extSrc = R"(
fn greet(name: String, prefix: String = "Hello") -> String {
    return "{prefix}, {strUpper(name)}!"
}

component Panel(title: String, gapSize: Int = 12) {
    Card(padding: 16, gap: gapSize) {
        Text(title)
        slot
    }
}

app "Transom3ExtensionsTest" (width: 400, height: 300) {
    persist state themeMode = "dark"
    state out = ""

    view Panel(title: "Settings") {
        Text("Mode: {themeMode}")
        Text(out)
        Button("TestAll") {
            let items = ["zeta", "alpha", "beta"]
            let joined = items |> listSort |> listReverse |> listJoin("-")
            let hasB = listContains(items, "beta")
            let sub = listSlice(items, 0, 2) |> listJoin(",")
            let g1 = greet("moaaz")
            let g2 = greet("ali", "Hi")
            out = "{g1}|{g2}|{joined}|{hasB}|{sub}"
            themeMode = "light"
        }
    }
}
)";
        DiagnosticEngine d2;
        auto cp2 = compileSource(extSrc, d2);
        check(!d2.hasErrors(), "slot + default params + persist state + list/str builtins compile cleanly: " + d2.formatAll());
        if (!d2.hasErrors()) {
            MockBackend b2;
            AppRuntime rt2(b2);
            rt2.mount(std::move(cp2));
            check(b2.findByKindAndProp("Text", "text", "Settings") != kInvalidWidget,
                  "Component slot renders wrapper Text('Settings')");
            WidgetHandle btn2 = b2.findByKindAndProp("Button", "label", "TestAll");
            check(btn2 != kInvalidWidget, "Component slot injects caller's Button('TestAll') inside Panel");
            if (btn2 != kInvalidWidget) {
                b2.nodes[btn2].onClick();
                std::string expected = "Hello, MOAAZ!|Hi, ALI!|zeta-beta-alpha|true|zeta,alpha";
                check(rt2.signals().get("out").toString() == expected,
                      "Default params + listSort/listReverse/listJoin/listContains/listSlice/strUpper work (got " +
                          rt2.signals().get("out").toString() + ")");
                // Re-mount a fresh runtime to verify `persist state themeMode` restored "light"
                DiagnosticEngine d3;
                auto cp3 = compileSource(extSrc, d3);
                MockBackend b3;
                AppRuntime rt3(b3);
                rt3.mount(std::move(cp3));
                check(rt3.signals().get("themeMode").toString() == "light",
                      "persist state automatically saves and restores 'light' across restarts");
                // Reset back to dark so test is idempotent
                rt3.signals().setState("themeMode", Value("dark"));
            }
        }
    }
}

int main() {
    testLexerInterpolation();
    testStaticTypeErrors();
    testReactiveCounterEndToEnd();
    testComponentsAndTwoWayBinding();
    testDynamicTodoApp();
    testModuleImportsAndRichDiagnostics();
    testSystemApisTimersAndUiPrimitives();
    testBytecodeSerializationRoundTrip();
    testNativePluginEndToEnd();
    testTplugOfficialModulesAndSecurity();
    testTstyleDesignSystem();
    testRestApiJsonPath();
    testPortableLayoutEngineGolden();
    testLayoutSpacerAndFill();
    testEnumAndMatch();
    testCoreLanguageCompleteness();

    std::cout << "Transom 2.0 tests: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
