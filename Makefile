CXX      = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2 -Isrc -Iinclude -MMD -MP
OBJCXX   = clang++
OBJFLAGS = -std=c++20 -Wall -Wextra -O2 -Isrc -Iinclude -ObjC++ -MMD -MP
LDFLAGS  = -framework Cocoa

CORE_SRCS = \
	src/frontend/lexer.cpp \
	src/frontend/parser.cpp \
	src/frontend/module_loader.cpp \
	src/plugin/plugin_manager.cpp \
	src/plugin/native_manifest.cpp \
	src/sema/builtin_widgets.cpp \
	src/sema/type_checker.cpp \
	src/stdlib/system_ops.cpp \
	src/style/style_parser.cpp \
	src/style/style_checker.cpp \
	src/layout/layout_engine.cpp \
	src/vm/compiler.cpp \
	src/vm/serializer.cpp \
	src/vm/vm.cpp \
	src/ui/app_runtime.cpp

PLATFORM_SRC = src/platform/macos/cocoa_backend.mm
MAIN_SRC     = src/main.cpp
TEST_SRC     = tests/test_main.cpp

BUILD_DIR = build
CORE_OBJS = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))
PLAT_OBJ  = $(BUILD_DIR)/platform/macos/cocoa_backend.o
MAIN_OBJ  = $(BUILD_DIR)/main.o
TEST_OBJ  = $(BUILD_DIR)/tests/test_main.o
DEPS      = $(CORE_OBJS:.o=.d) $(PLAT_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

BIN      = transom
TEST_BIN = $(BUILD_DIR)/transom_tests

all: $(BIN)

# Test-only native plugin fixture (domain-neutral). Not part of the core build.
FIXTURE_LIB = tests/fixtures/plugins/demo/libdemo.dylib

fixtures: $(FIXTURE_LIB)

$(FIXTURE_LIB): tests/fixtures/plugins/demo/demo.c include/transom_plugin.h
	clang -Wall -Wextra -O2 -Iinclude -dynamiclib $< -o $@

$(BIN): $(CORE_OBJS) $(PLAT_OBJ) $(MAIN_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(CORE_OBJS) $(TEST_OBJ)
	$(CXX) $^ -o $@

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PLAT_OBJ): $(PLATFORM_SRC)
	@mkdir -p $(dir $@)
	$(OBJCXX) $(OBJFLAGS) -c $< -o $@

$(TEST_OBJ): $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: fixtures $(TEST_BIN)
	./$(TEST_BIN)

run: $(BIN)
	./$(BIN) run examples/counter.transom

clean:
	rm -rf $(BUILD_DIR) $(BIN) $(FIXTURE_LIB)

install: $(BIN)
	@mkdir -p "$$HOME/.local/bin"
	@ln -sfn "$(CURDIR)/$(BIN)" "$$HOME/.local/bin/transom"
	@echo "Installed global CLI -> $$HOME/.local/bin/transom"

install-vscode:
	@for dir in "$$HOME/.vscode/extensions" "$$HOME/.antigravity-ide/extensions"; do \
		if [ -d "$$dir" ]; then \
			dest="$$dir/transom.transom-language-3.0.0"; \
			rm -rf "$$dest" "$$dir/transom.transom-language-2.0.0" "$$dir/transom-language"; \
			cp -R "$(CURDIR)/editors/vscode" "$$dest"; \
			if [ -f "$$dir/extensions.json" ]; then \
				node -e 'const fs=require("fs"), p=process.argv[1], d=process.argv[2]; \
				let arr=JSON.parse(fs.readFileSync(p,"utf8")); \
				arr=arr.filter(e=>!(e.identifier && e.identifier.id==="transom.transom-language")); \
				arr.push({identifier:{id:"transom.transom-language"},version:"3.0.0",location:{$$mid:1,path:d,scheme:"file"},relativeLocation:"transom.transom-language-3.0.0",metadata:{installedTimestamp:Date.now(),source:"vsix"}}); \
				fs.writeFileSync(p, JSON.stringify(arr));' "$$dir/extensions.json" "$$dest"; \
			fi; \
			echo "Installed Transom 3.0 extension into $$dest"; \
		fi; \
	done

-include $(DEPS)

.PHONY: all fixtures test run clean install install-vscode

cross-windows:
	@mkdir -p $(BUILD_DIR)/win
	x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Iinclude \
		$(CORE_SRCS) src/platform/windows/win32_d2d_backend.cpp src/main.cpp \
		-o $(BUILD_DIR)/win/transom.exe \
		-static-libgcc -static-libstdc++ -luser32 -lgdi32 -ld2d1 -ldwrite -lwinhttp -lcomdlg32 -lshell32 -lole32 -luuid
	@echo "Cross-compiled Windows binary: $(BUILD_DIR)/win/transom.exe"
