cmake_minimum_required(VERSION 3.20)
project(Transom VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Portable core library (NO platform UI, NO domain-specific frameworks)
set(TRANSOM_CORE_SOURCES
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/frontend/module_loader.cpp
    src/plugin/plugin_manager.cpp
    src/plugin/native_manifest.cpp
    src/sema/builtin_widgets.cpp
    src/sema/type_checker.cpp
    src/stdlib/system_ops.cpp
    src/style/style_parser.cpp
    src/style/style_checker.cpp
    src/layout/layout_engine.cpp
    src/vm/compiler.cpp
    src/vm/serializer.cpp
    src/vm/vm.cpp
    src/ui/app_runtime.cpp
)

add_library(transom-core STATIC ${TRANSOM_CORE_SOURCES})
target_include_directories(transom-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(transom-core PRIVATE /W4)
else()
    target_compile_options(transom-core PRIVATE -Wall -Wextra)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(transom-core PUBLIC dl)
endif()

# Platform native backend
if(APPLE)
    enable_language(OBJCXX)
    set(TRANSOM_PLATFORM_SRC src/platform/macos/cocoa_backend.mm)
    set_source_files_properties(${TRANSOM_PLATFORM_SRC} PROPERTIES COMPILE_FLAGS "-ObjC++")
elseif(WIN32)
    set(TRANSOM_PLATFORM_SRC src/platform/windows/win32_d2d_backend.cpp)
else()
    message(FATAL_ERROR "Unsupported desktop platform for full 'transom' binary. Build 'transom-core' and 'transom_tests' instead.")
endif()

add_executable(transom src/main.cpp ${TRANSOM_PLATFORM_SRC})
target_link_libraries(transom PRIVATE transom-core)

if(APPLE)
    target_link_libraries(transom PRIVATE "-framework Cocoa")
elseif(WIN32)
    target_link_libraries(transom PRIVATE
        user32 gdi32 d2d1 dwrite winhttp comdlg32 shell32 ole32 uuid)
    if(MINGW)
        target_link_options(transom PRIVATE -municode)
    endif()
endif()

# Headless core + layout + compiler + VM test suite
enable_testing()
add_executable(transom_tests tests/test_main.cpp)
target_link_libraries(transom_tests PRIVATE transom-core)
add_test(NAME transom_core_tests COMMAND transom_tests)
