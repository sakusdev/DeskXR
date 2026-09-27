cmake_minimum_required(VERSION 3.22.1)

project(deskxr_quest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenXR REQUIRED CONFIG)

add_library(deskxr_quest SHARED
    deskxr_quest.cpp
)

target_link_libraries(deskxr_quest
    PRIVATE
        OpenXR::openxr_loader
        android
        log
        EGL
        GLESv3
)

target_compile_definitions(deskxr_quest
    PRIVATE
        XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_OPENGL_ES
)
