# Transom 3.0 Core

A **domain-free, cross-platform declarative reactive programming language** for building lightweight native desktop applications (macOS AppKit + Windows Win32/Direct2D) — powered by a two-pass Layout Engine, a Bytecode VM, fine-grained reactive signals, `.tstyle` design systems, and a strict `.tplug` / `.tnative` (C ABI v2) plugin architecture.

Zero Electron. Zero Node.js. Zero external runtime dependencies.

```transom
component Panel(title: String, bg: String = "#1e2330") {
    Card(padding: 20, bg: bg, radius: 14, gap: 12) {
        Text(title, size: 13, weight: "semibold", color: "#94a3b8")
        slot
    }
}

app "Transom Counter" (width: 420, height: 320, bg: "#0f121a") {
    persist state count = 0
    derived label = if count == 0 { "Zero" } else { "Value: {count}" }

    view VStack(gap: 18, padding: 28, align: "center") {
        Panel(title: "AUTO-SAVED COUNTER") {
            Text("{count}", size: 44, weight: "bold", color: "#38bdf8", align: "center")
            Text(label, size: 13, color: "#94a3b8", align: "center")
        }
        HStack(gap: 10) {
            Button("-1") { count -= 1 }
            Button("+1", style: "primary") { count += 1 }
        }
    }
}
```

## Quick Start

```bash
make                                             # Build ./transom (macOS C++20 + Cocoa)
make test                                        # Run all 180 unit, layout & language tests
bash scripts/check_core_independence.sh          # Verify core architectural isolation

./transom run examples/counter.transom           # Launch Persistent Reactive Counter
./transom run examples/todo.transom              # Launch Reactive Todo App (enum + match)
./transom run examples/converter.transom         # Launch Temperature Studio (Grid + Slider)
./transom run examples/themed_app.transom        # Launch .tstyle Theme Switcher
./transom run examples/api_demo.transom          # Launch GitHub API Explorer
./transom watch examples/themed_app.transom      # Live Hot Reload on save
./transom check examples/todo.transom            # Static syntax & type check without UI
./transom build examples/todo.transom --out Tasks.app
```

### Cross-Platform CMake & Windows Build

```bash
cmake --preset macos-release && cmake --build --preset macos-release
make cross-windows                               # Cross-compile build/win/transom.exe via MinGW-w64
./transom build examples/counter.transom --target windows --out Counter.exe
```

## Core Architecture

```
src/
├── frontend/   Lexer, Parser (error recovery), AST, Module Loader
├── sema/       Static Type Checker, Built-in WidgetSchemaRegistry
├── style/      .tstyle Parser, StyleChecker, Design Tokens & Live Themes
├── layout/     Portable Two-Pass Layout Engine (Measure / Arrange + RTL)
├── reactive/   Fine-grained SignalGraph (state, persist state, derived)
├── vm/         Bytecode Compiler, .tbc Serializer, Stack VM
├── plugin/     .tplug Capability Sandbox + .tnative C ABI v2 Loader (SHA-256 lock)
├── stdlib/     Cross-platform File I/O, App Storage, JSON, SHA-256, SemVer
├── ui/         AppRuntime (dynamic if/for views, timers, Hot Reload)
└── platform/   Abstract PlatformBackend → macos/cocoa_backend.mm | windows/win32_d2d_backend.cpp
include/
└── transom_plugin.h   Frozen C ABI v2 header for external native plugins
```

## Key Language Features

- **Reactive State**: `state`, `persist state` (auto-saved), `derived`, `every(ms) { ... }`
- **Types & Control Flow**: `Int`, `Float`, `Bool`, `String`, `Bytes`, `List<T>`, `Map<String, T>`, `Result<T>`, `struct`, `enum`, exhaustive `match`, `for`/`range`/`break`/`continue`, `const`, `??`, `|>`
- **Reusable UI**: `component` with **default parameters** and **`slot`** children
- **Layout & Widgets**: `VStack`, `HStack`, `Grid`, `ZStack`, `Modal`, `ScrollView`, `Card`, `Spacer`, `Divider`, `Icon`, `Text`, `Button`, `Input`, `Slider`, `Toggle`, `Select`, `Image`, `Progress`, `Canvas`
- **Extensible Plugin Boundary**: Sandboxed `.tplug` modules and SHA-256 pinned `.tnative` plugins (`transom add`, `transom plugin check`, `transom plugin lock`)
