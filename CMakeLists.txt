cmake_minimum_required(VERSION 3.16)

# =============================================================================
# Version
# =============================================================================
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

project(smart-camera
    VERSION ${PROJECT_VERSION}
    LANGUAGES CXX
)

# =============================================================================
# C++17 Standard
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Git commit hash
# =============================================================================
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0)
        set(GIT_COMMIT_HASH "unknown")
    endif()
else()
    set(GIT_COMMIT_HASH "unknown")
endif()

message(STATUS "Version: ${PROJECT_VERSION} (${GIT_COMMIT_HASH})")

# =============================================================================
# Platform detection
# =============================================================================
if(APPLE)
    set(PLATFORM_MACOS ON)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(PLATFORM_MACOS_X86_64 ON)
        message(STATUS "Platform: macOS x86_64 (Intel)")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(PLATFORM_MACOS_AARCH64 ON)
        message(STATUS "Platform: macOS aarch64 (Apple Silicon)")
    endif()
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_LINUX ON)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(PLATFORM_LINUX_AARCH64 ON)
        message(STATUS "Platform: Linux aarch64 (Raspberry Pi)")
    else()
        message(STATUS "Platform: Linux ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()

# =============================================================================
# Conditional compilation macros
# =============================================================================

# KVS WebRTC SDK (optional, typically Linux only)
# Try find_package first, then fall back to manual path detection
find_package(KVSWebRTC QUIET)
if(NOT KVSWebRTC_FOUND)
    # Search common install locations
    set(_KVS_SDK_SEARCH_PATHS
        /opt/amazon-kinesis-video-streams-webrtc-sdk-c
        /usr/local
    )
    foreach(_path ${_KVS_SDK_SEARCH_PATHS})
        if(EXISTS "${_path}/include/com/amazonaws/kinesis/video/webrtcclient/Include.h")
            set(KVS_WEBRTC_SDK_DIR "${_path}")
            break()
        endif()
        # Also check build output directory structure
        if(EXISTS "${_path}/build/libkvsWebrtcClient.so" OR EXISTS "${_path}/build/libkvsWebrtcClient.a")
            set(KVS_WEBRTC_SDK_DIR "${_path}")
            break()
        endif()
    endforeach()

    if(KVS_WEBRTC_SDK_DIR)
        set(KVSWebRTC_FOUND TRUE)
        # Set include paths — SDK headers are split across src/include and open-source/include
        set(KVS_WEBRTC_INCLUDE_DIRS
            "${KVS_WEBRTC_SDK_DIR}/src/include"
            "${KVS_WEBRTC_SDK_DIR}/open-source/include"
        )
        # Find the library
        find_library(KVS_WEBRTC_LIB kvsWebrtcClient
            PATHS "${KVS_WEBRTC_SDK_DIR}/build" "${KVS_WEBRTC_SDK_DIR}/lib"
            NO_DEFAULT_PATH)
        find_library(KVS_WEBRTC_SIGNALING_LIB kvsWebrtcSignalingClient
            PATHS "${KVS_WEBRTC_SDK_DIR}/build" "${KVS_WEBRTC_SDK_DIR}/lib"
            NO_DEFAULT_PATH)
    endif()
endif()

if(KVSWebRTC_FOUND)
    # HAS_KVS_WEBRTC_SDK is set as PUBLIC on webrtc_agent target (propagates to dependents)
    if(KVS_WEBRTC_SDK_DIR)
        message(STATUS "KVS WebRTC SDK: Found (${KVS_WEBRTC_SDK_DIR})")
    else()
        message(STATUS "KVS WebRTC SDK: Found")
    endif()
else()
    message(STATUS "KVS WebRTC SDK: Not found (stub mode)")
endif()

# libcamera (Linux aarch64 only)
if(PLATFORM_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBCAMERA QUIET libcamera)
    endif()
    if(LIBCAMERA_FOUND)
        add_compile_definitions(HAS_LIBCAMERA)
        message(STATUS "libcamera: Found")
    else()
        message(STATUS "libcamera: Not found")
    endif()
endif()

# V4L2 (Linux only)
if(PLATFORM_LINUX)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("linux/videodev2.h" HAS_V4L2_HEADER)
    if(HAS_V4L2_HEADER)
        add_compile_definitions(HAS_V4L2)
        message(STATUS "V4L2: Found")
    else()
        message(STATUS "V4L2: Not found")
    endif()
endif()

# systemd (Linux only)
if(PLATFORM_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SYSTEMD QUIET libsystemd)
    endif()
    if(SYSTEMD_FOUND)
        add_compile_definitions(HAS_SYSTEMD)
        message(STATUS "systemd: Found")
    else()
        message(STATUS "systemd: Not found")
    endif()
endif()

# =============================================================================
# Version and Git hash compile definitions
# =============================================================================
add_compile_definitions(
    PROJECT_VERSION="${PROJECT_VERSION}"
    PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    GIT_COMMIT_HASH="${GIT_COMMIT_HASH}"
)

# =============================================================================
# Dependencies via find_package / pkg-config
# Dependencies are optional at this stage — required when building full app
# =============================================================================
find_package(PkgConfig QUIET)

# GStreamer (optional — needed for pipeline modules)
if(PkgConfig_FOUND)
    pkg_check_modules(GSTREAMER QUIET gstreamer-1.0)
    pkg_check_modules(GSTREAMER_APP QUIET gstreamer-app-1.0)
endif()
if(GSTREAMER_FOUND)
    add_compile_definitions(HAS_GSTREAMER)
    message(STATUS "GStreamer: ${GSTREAMER_VERSION}")
else()
    message(STATUS "GStreamer: Not found (pipeline modules disabled)")
endif()

# OpenSSL (optional — needed for auth modules)
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
else()
    message(STATUS "OpenSSL: Not found (auth modules disabled)")
endif()

# CURL (optional — needed for auth and connection monitor)
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "CURL: ${CURL_VERSION_STRING}")
else()
    message(STATUS "CURL: Not found (network modules disabled)")
endif()

# =============================================================================
# Include directories
# =============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)
if(GSTREAMER_FOUND)
    include_directories(${GSTREAMER_INCLUDE_DIRS})
    if(GSTREAMER_APP_FOUND)
        include_directories(${GSTREAMER_APP_INCLUDE_DIRS})
    endif()
endif()

# =============================================================================
# Google Test & RapidCheck (FetchContent)
# =============================================================================
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent overriding parent project compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        ff6af6fc683159deb51c543b065eba14dfcf329b
)
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest rapidcheck)

enable_testing()

# =============================================================================
# Source files (populated incrementally as modules are added)
# =============================================================================
# Core headers are header-only for now; library targets added as implementations grow.

# Config Manager library
add_library(config_manager STATIC src/config/config_manager.cpp)
target_include_directories(config_manager PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Log Manager library
add_library(log_manager STATIC src/logging/log_manager.cpp)
target_include_directories(log_manager PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Camera Source library
set(CAMERA_SOURCE_SRCS
    src/camera/camera_source_factory.cpp
    src/camera/videotestsrc_source.cpp
)
if(PLATFORM_LINUX AND LIBCAMERA_FOUND)
    list(APPEND CAMERA_SOURCE_SRCS src/camera/libcamera_source.cpp)
endif()
if(PLATFORM_LINUX AND HAS_V4L2_HEADER)
    list(APPEND CAMERA_SOURCE_SRCS src/camera/v4l2_source.cpp)
endif()

add_library(camera_source STATIC ${CAMERA_SOURCE_SRCS})
target_include_directories(camera_source PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(camera_source PUBLIC config_manager)
if(PLATFORM_LINUX AND LIBCAMERA_FOUND)
    target_compile_definitions(camera_source PRIVATE HAS_LIBCAMERA)
    target_include_directories(camera_source PRIVATE ${LIBCAMERA_INCLUDE_DIRS})
    target_link_libraries(camera_source PRIVATE ${LIBCAMERA_LIBRARIES})
endif()
if(PLATFORM_LINUX AND HAS_V4L2_HEADER)
    target_compile_definitions(camera_source PRIVATE HAS_V4L2)
endif()

# GStreamer Pipeline library
add_library(gstreamer_pipeline STATIC src/pipeline/gstreamer_pipeline.cpp)
target_include_directories(gstreamer_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gstreamer_pipeline PUBLIC config_manager)
if(GSTREAMER_FOUND)
    target_compile_definitions(gstreamer_pipeline PRIVATE HAS_GSTREAMER)
    target_include_directories(gstreamer_pipeline PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(gstreamer_pipeline PRIVATE ${GSTREAMER_LIBRARIES})
    if(GSTREAMER_APP_FOUND)
        target_include_directories(gstreamer_pipeline PRIVATE ${GSTREAMER_APP_INCLUDE_DIRS})
        target_link_libraries(gstreamer_pipeline PRIVATE ${GSTREAMER_APP_LIBRARIES})
    endif()
endif()

# Stream Uploader library
add_library(stream_uploader STATIC src/stream/stream_uploader.cpp)
target_include_directories(stream_uploader PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(stream_uploader PUBLIC config_manager)
if(GSTREAMER_FOUND)
    target_compile_definitions(stream_uploader PRIVATE HAS_GSTREAMER)
    target_include_directories(stream_uploader PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(stream_uploader PRIVATE ${GSTREAMER_LIBRARIES})
endif()

# IoT Authenticator library
add_library(iot_authenticator STATIC src/auth/iot_authenticator.cpp)
target_include_directories(iot_authenticator PUBLIC ${CMAKE_SOURCE_DIR}/include)
if(OPENSSL_FOUND)
    target_link_libraries(iot_authenticator PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(iot_authenticator PRIVATE OPENSSL_FOUND)
endif()
if(CURL_FOUND)
    target_link_libraries(iot_authenticator PUBLIC CURL::libcurl)
    target_compile_definitions(iot_authenticator PRIVATE CURL_FOUND)
endif()

# Bitrate Controller library
add_library(bitrate_controller STATIC src/control/bitrate_controller.cpp)
target_include_directories(bitrate_controller PUBLIC ${CMAKE_SOURCE_DIR}/include)

# WebRTC Agent library
add_library(webrtc_agent STATIC src/webrtc/webrtc_agent.cpp)
target_include_directories(webrtc_agent PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(webrtc_agent PUBLIC config_manager)
if(KVSWebRTC_FOUND)
    target_compile_definitions(webrtc_agent PUBLIC HAS_KVS_WEBRTC_SDK)
    if(TARGET KVSWebRTC::KVSWebRTC)
        # SDK installed with CMake config — use imported target
        target_link_libraries(webrtc_agent PUBLIC KVSWebRTC::KVSWebRTC)
    elseif(KVS_WEBRTC_LIB)
        # SDK found via manual path detection — PUBLIC because header includes SDK types
        target_include_directories(webrtc_agent PUBLIC ${KVS_WEBRTC_INCLUDE_DIRS})
        target_link_libraries(webrtc_agent PUBLIC ${KVS_WEBRTC_LIB})
        if(KVS_WEBRTC_SIGNALING_LIB)
            target_link_libraries(webrtc_agent PUBLIC ${KVS_WEBRTC_SIGNALING_LIB})
        endif()
    endif()
endif()

# Stream Mode FSM library
add_library(stream_mode_fsm STATIC src/core/stream_mode_fsm.cpp)
target_include_directories(stream_mode_fsm PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Connection Monitor library
add_library(connection_monitor STATIC src/monitor/connection_monitor.cpp)
target_include_directories(connection_monitor PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(connection_monitor PUBLIC stream_mode_fsm)
if(CURL_FOUND)
    target_link_libraries(connection_monitor PRIVATE CURL::libcurl)
    target_compile_definitions(connection_monitor PRIVATE CURL_FOUND)
endif()

# Health Monitor library
add_library(health_monitor STATIC src/monitor/health_monitor.cpp)
target_include_directories(health_monitor PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(health_monitor PUBLIC gstreamer_pipeline)

# Watchdog library
add_library(watchdog STATIC src/monitor/watchdog.cpp)
target_include_directories(watchdog PUBLIC ${CMAKE_SOURCE_DIR}/include)
if(SYSTEMD_FOUND)
    target_compile_definitions(watchdog PRIVATE HAS_SYSTEMD)
    target_link_libraries(watchdog PRIVATE ${SYSTEMD_LIBRARIES})
endif()

# Video Producer library
add_library(video_producer STATIC src/pipeline/video_producer.cpp)
target_include_directories(video_producer PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Frame Buffer Pool library
add_library(frame_buffer_pool STATIC src/buffer/frame_buffer_pool.cpp)
target_include_directories(frame_buffer_pool PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Pipeline Orchestrator library
add_library(pipeline_orchestrator STATIC src/pipeline/pipeline_orchestrator.cpp)
target_include_directories(pipeline_orchestrator PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pipeline_orchestrator PUBLIC gstreamer_pipeline health_monitor)

# Resource Guard library
add_library(resource_guard STATIC src/core/resource_guard.cpp)
target_include_directories(resource_guard PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Shutdown Handler library
add_library(shutdown_handler STATIC src/core/shutdown_handler.cpp)
target_include_directories(shutdown_handler PUBLIC ${CMAKE_SOURCE_DIR}/include)

# AI Pipeline library
add_library(ai_pipeline STATIC src/ai/ai_pipeline.cpp)
target_include_directories(ai_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ai_pipeline PUBLIC frame_buffer_pool)

# =============================================================================
# Main executable
# =============================================================================
add_executable(smart-camera src/main.cpp)
target_include_directories(smart-camera PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(smart-camera PRIVATE
    config_manager
    log_manager
    iot_authenticator
    camera_source
    gstreamer_pipeline
    stream_uploader
    webrtc_agent
    bitrate_controller
    connection_monitor
    health_monitor
    watchdog
    frame_buffer_pool
    ai_pipeline
    pipeline_orchestrator
    resource_guard
    shutdown_handler
    video_producer
    stream_mode_fsm
)

# =============================================================================
# Tests
# =============================================================================
add_subdirectory(tests)
