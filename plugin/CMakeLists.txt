cmake_minimum_required(VERSION 3.20)
project(LeoProximityChat VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static MSVC runtime (/MT) so the DLL has no CRT DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Force vcpkg to use static triplet
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

# ─── BakkesMod SDK ────────────────────────────────────────────────────────────
# Set BAKKESMOD_SDK_PATH to the BakkesMod SDK root (contains "include/" and "lib/")
# Typical: C:/Program Files (x86)/Steam/steamapps/common/rocketleague/Binaries/Win64/bakkesmod/bakkesmodsdk
if(NOT DEFINED BAKKESMOD_SDK_PATH)
    # Try common locations
    set(_BAKKESMOD_CANDIDATES
        "$ENV{APPDATA}/bakkesmod/bakkesmod/bakkesmodsdk"
        "$ENV{BAKKESMOD_PATH}/bakkesmodsdk"
        "C:/Program Files (x86)/Steam/steamapps/common/rocketleague/Binaries/Win64/bakkesmod/bakkesmodsdk"
    )
    foreach(_CANDIDATE ${_BAKKESMOD_CANDIDATES})
        if(EXISTS "${_CANDIDATE}/include")
            set(BAKKESMOD_SDK_PATH "${_CANDIDATE}")
            message(STATUS "Found BakkesMod SDK at: ${BAKKESMOD_SDK_PATH}")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED BAKKESMOD_SDK_PATH OR NOT EXISTS "${BAKKESMOD_SDK_PATH}/include")
    message(FATAL_ERROR 
        "BakkesMod SDK not found. Set BAKKESMOD_SDK_PATH to the SDK directory.\n"
        "Example: cmake -DBAKKESMOD_SDK_PATH=\"C:/path/to/bakkesmodsdk\" .."
    )
endif()

# Normalize path to use forward slashes (avoids CMake escape issues)
file(TO_CMAKE_PATH "${BAKKESMOD_SDK_PATH}" BAKKESMOD_SDK_PATH)

# ─── Dependencies (via vcpkg) ────────────────────────────────────────────────
find_package(portaudio CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ─── ImGui sources from BakkesMod SDK (must be compiled into the plugin) ─────
set(IMGUI_SOURCES
    "${BAKKESMOD_SDK_PATH}/include/imgui/imgui.cpp"
    "${BAKKESMOD_SDK_PATH}/include/imgui/imgui_draw.cpp"
    "${BAKKESMOD_SDK_PATH}/include/imgui/imgui_widgets.cpp"
)

# ─── Sources ─────────────────────────────────────────────────────────────────
set(PLUGIN_SOURCES
    src/pch.cpp
    src/VoiceCodec.cpp
    src/AudioEngine.cpp
    src/SpatialAudio.cpp
    src/NetworkManager.cpp
    src/LeoProximityChat.cpp
    ${IMGUI_SOURCES}
)

set(PLUGIN_HEADERS
    src/pch.h
    src/version.h
    src/ThreadSafeQueue.h
    src/Protocol.h
    src/VoiceCodec.h
    src/AudioEngine.h
    src/SpatialAudio.h
    src/NetworkManager.h
    src/LeoProximityChat.h
)

# ─── DLL Target ──────────────────────────────────────────────────────────────
add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_precompile_headers(${PROJECT_NAME} PRIVATE src/pch.h)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${BAKKESMOD_SDK_PATH}/include"
    "${BAKKESMOD_SDK_PATH}/include/imgui"
    src
)

target_link_directories(${PROJECT_NAME} PRIVATE
    "${BAKKESMOD_SDK_PATH}/lib"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    pluginsdk
    portaudio_static
    Opus::opus
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
)

# Ensure static runtime for all sources
set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows-specific
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        PLUGIN_EXPORTS
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32
        winmm
        bcrypt
    )
endif()

# ─── Output ──────────────────────────────────────────────────────────────────
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "LeoProximityChat"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ─── Install (copy to BakkesMod plugins directory) ───────────────────────────
set(BAKKESMOD_PLUGINS_DIR "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins" CACHE PATH "BakkesMod plugins directory")
set(BAKKESMOD_SETTINGS_DIR "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins/settings" CACHE PATH "BakkesMod settings directory")

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "${BAKKESMOD_PLUGINS_DIR}"
    LIBRARY DESTINATION "${BAKKESMOD_PLUGINS_DIR}"
)

install(FILES "${CMAKE_SOURCE_DIR}/settings/leo_proximity_chat.set"
    DESTINATION "${BAKKESMOD_SETTINGS_DIR}"
)

# ─── Convenience: post-build copy ────────────────────────────────────────────
option(AUTO_DEPLOY "Auto-copy DLL to BakkesMod plugins folder after build" ON)

if(AUTO_DEPLOY)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${BAKKESMOD_PLUGINS_DIR}/LeoProximityChat.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/settings/leo_proximity_chat.set"
            "${BAKKESMOD_SETTINGS_DIR}/leo_proximity_chat.set"
        COMMENT "Deploying LeoProximityChat to BakkesMod plugins directory"
    )
endif()
