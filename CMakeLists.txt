cmake_minimum_required(VERSION 3.20)
project(NetMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000)

# MSVC UTF-8 source and execution charset
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ── FetchContent ──────────────────────────────────────────────────────────────
include(FetchContent)

# Google Test
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# nlohmann/json (header-only, bundled in third_party/)
set(NLOHMANN_JSON_DIR "${CMAKE_SOURCE_DIR}/third_party/nlohmann")

# ── WinDivert ──
set(WINDIVERT_DIR "${CMAKE_SOURCE_DIR}/third_party/WinDivert")

# ── Core library (shared between app and tests) ──
add_library(NetMonitorCore STATIC
    src/core/TokenBucket.cpp
    src/core/TrafficTracker.cpp
    src/core/AlertManager.cpp
    src/core/EtwMonitor.cpp
    src/core/DivertLimiter.cpp
    src/core/TrafficLogger.cpp
    src/core/ConfigStore.cpp
)
target_include_directories(NetMonitorCore PUBLIC src "${WINDIVERT_DIR}/include" "${NLOHMANN_JSON_DIR}/..")
target_link_libraries(NetMonitorCore PUBLIC
    "${WINDIVERT_DIR}/lib/WinDivert.lib"
    iphlpapi ws2_32 psapi tdh
)

# ── Main application ──
add_executable(NetMonitor WIN32
    src/main.cpp
    src/gui/MainWindow.cpp
    src/gui/Dialogs.cpp
    src/resource.rc
)
target_link_libraries(NetMonitor PRIVATE NetMonitorCore comctl32)

# Embed manifest for admin privileges
if(MSVC)
    set_target_properties(NetMonitor PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
    )
endif()

# Copy WinDivert runtime files next to the executable
add_custom_command(TARGET NetMonitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/bin/WinDivert.dll"
        "$<TARGET_FILE_DIR:NetMonitor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINDIVERT_DIR}/bin/WinDivert64.sys"
        "$<TARGET_FILE_DIR:NetMonitor>"
)

# ── Tests ──
enable_testing()
add_executable(NetMonitorTests
    tests/test_main.cpp
    tests/test_token_bucket.cpp
    tests/test_traffic_tracker.cpp
    tests/test_alert_manager.cpp
    tests/test_divert_limiter.cpp
)
target_link_libraries(NetMonitorTests PRIVATE NetMonitorCore GTest::gtest GTest::gtest_main)
add_test(NAME NetMonitorTests COMMAND NetMonitorTests)
