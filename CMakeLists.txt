cmake_minimum_required(VERSION 3.20)
project(BloodTrailTogether VERSION 0.6.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no VC++ redist needed by users

if(NOT WIN32)
  message(FATAL_ERROR "BloodTrail Together targets Windows only")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  BTT_VERSION="${PROJECT_VERSION}" BTT_VERSION_W=L"${PROJECT_VERSION}")
option(BTT_RELEASE_BUILD "Public release: hide developer-only features" OFF)
if(BTT_RELEASE_BUILD)
  add_compile_definitions(BTT_RELEASE=1)
endif()
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

# Core library: no Unreal, no UE4SS dependencies. Shared by the game DLL, SimPeer and tests.
add_library(btt_lib STATIC
  src/core/log.cpp
  src/core/config.cpp
  src/core/util.cpp
  src/net/loopback_transport.cpp
  src/net/udp_transport.cpp
  src/net/impairment.cpp
  src/net/session.cpp
  src/relay/hmac.cpp
  src/relay/json.cpp
  src/relay/http.cpp
  src/relay/relay_messages.cpp
  src/relay/relay_api.cpp
  src/relay/relay_transport.cpp
  src/relay/lobby.cpp
  src/sync/pose_codec.cpp
  src/sync/interp.cpp
  src/engine/net_engine.cpp
  src/engine/update_check.cpp
)
target_include_directories(btt_lib PUBLIC src)
target_link_libraries(btt_lib PUBLIC ws2_32 winhttp bcrypt winmm)

# The DLL loaded by the UE4SS Lua mod (package.loadlib(path, "*")) inside the game process.
add_library(btt_core SHARED
  src/bridge/dllmain.cpp
  src/bridge/pipe_server.cpp
  src/bridge/lua_bridge.cpp
  src/bridge/control_window.cpp
)
target_link_libraries(btt_core PRIVATE btt_lib user32 gdi32 gdiplus shell32 comctl32)

add_executable(btt_simpeer tools/simpeer/main.cpp)
target_link_libraries(btt_simpeer PRIVATE btt_lib)

add_executable(btt_tests tests/test_main.cpp)
target_link_libraries(btt_tests PRIVATE btt_lib)

# Installer (GUI + CLI). The payload (mod files, optionally UE4SS) is embedded as RCDATA 101.
set(BTT_PAYLOAD "" CACHE FILEPATH "payload.bin produced by tools/make_release.py")
set(INSTALLER_RC ${CMAKE_BINARY_DIR}/installer.rc)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/installer/installer.manifest" _manifest)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/app.ico" _icon)
set(_rc "1 24 \"${_manifest}\"\n1 ICON \"${_icon}\"\n")
if(BTT_PAYLOAD)
  file(TO_CMAKE_PATH "${BTT_PAYLOAD}" _payload)
  string(APPEND _rc "101 RCDATA \"${_payload}\"\n")
endif()
file(WRITE ${INSTALLER_RC} "${_rc}")
add_executable(btt_installer WIN32 installer/installer_main.cpp installer/install_core.cpp ${INSTALLER_RC})
target_link_libraries(btt_installer PRIVATE comctl32 shell32 ole32 version advapi32 user32 gdi32)
target_link_options(btt_installer PRIVATE /MANIFEST:NO)
if(BTT_PAYLOAD)
  set_source_files_properties(${INSTALLER_RC} PROPERTIES OBJECT_DEPENDS "${BTT_PAYLOAD}")
endif()

# Pipe client that emulates the Lua side, for testing the DLL bridge without the game.
add_executable(btt_bridge_probe tools/bridge_probe/main.cpp)
target_link_libraries(btt_bridge_probe PRIVATE btt_lib)
