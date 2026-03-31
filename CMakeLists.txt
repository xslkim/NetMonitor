cmake_minimum_required(VERSION 3.20)
project(NetMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000000)

# MSVC UTF-8 source and execution charset
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ── Google Test via FetchContent ──
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Core library (shared between app and tests) ──
add_library(NetMonitorCore STATIC
    src/core/TokenBucket.cpp
    src/core/TrafficTracker.cpp
    src/core/AlertManager.cpp
    src/core/EtwMonitor.cpp
    src/core/WfpLimiter.cpp
)
target_include_directories(NetMonitorCore PUBLIC src)
target_link_libraries(NetMonitorCore PUBLIC
    iphlpapi ws2_32 fwpuclnt psapi tdh
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

# ── Tests ──
enable_testing()
add_executable(NetMonitorTests
    tests/test_main.cpp
    tests/test_token_bucket.cpp
    tests/test_traffic_tracker.cpp
    tests/test_alert_manager.cpp
)
target_link_libraries(NetMonitorTests PRIVATE NetMonitorCore GTest::gtest GTest::gtest_main)
add_test(NAME NetMonitorTests COMMAND NetMonitorTests)
