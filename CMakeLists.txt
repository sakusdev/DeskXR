cmake_minimum_required(VERSION 3.20)

project(DeskXR VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The initial DeskXR SteamVR driver PoC currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG 0924064316de3effbcd1acf1e309182a2deb1c05
    GIT_SHALLOW FALSE
)
FetchContent_GetProperties(openvr)
if(NOT openvr_POPULATED)
    FetchContent_Populate(openvr)
endif()

add_library(driver_deskxr SHARED
    src/deskxr_driver.cpp
    src/protocol.hpp
)

target_include_directories(driver_deskxr PRIVATE
    "${openvr_SOURCE_DIR}/headers"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(driver_deskxr PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(driver_deskxr PRIVATE ws2_32)

if(MSVC)
    target_compile_options(driver_deskxr PRIVATE /W4 /permissive-)
endif()

set(DESKXR_DRIVER_ROOT "${CMAKE_BINARY_DIR}/deskxr")
set(DESKXR_DRIVER_BIN "${DESKXR_DRIVER_ROOT}/bin/win64")

set_target_properties(driver_deskxr PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${DESKXR_DRIVER_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DESKXR_DRIVER_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DESKXR_DRIVER_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DESKXR_DRIVER_BIN}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${DESKXR_DRIVER_BIN}"
)

add_custom_command(TARGET driver_deskxr POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DESKXR_DRIVER_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/driver.vrdrivermanifest"
        "${DESKXR_DRIVER_ROOT}/driver.vrdrivermanifest"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/resources"
        "${DESKXR_DRIVER_ROOT}/resources"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/install-driver.ps1"
        "${DESKXR_DRIVER_ROOT}/install.ps1"
    COMMENT "Packaging DeskXR SteamVR driver"
)
