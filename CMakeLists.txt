cmake_minimum_required(VERSION 3.20)
project(auto_dark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(auto-dark
  src/main.cpp
  src/json.cpp
  src/params.cpp
  src/shaders.cpp
  src/monitor.cpp
  src/http.cpp
  src/jpeg.cpp
  src/color.cpp
  src/tray.cpp
)

# Runs as a background tray app: no console window (use --console for one). Keep main() as entry.
set_target_properties(auto-dark PROPERTIES WIN32_EXECUTABLE TRUE)
if(MSVC)
  target_link_options(auto-dark PRIVATE /ENTRY:mainCRTStartup)
endif()

# Shaders, web UI and profiles are loaded from the source tree so they hot-reload while editing.
target_compile_definitions(auto-dark PRIVATE
  AUTO_DARK_ROOT="${CMAKE_SOURCE_DIR}"
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

if(MSVC)
  target_compile_options(auto-dark PRIVATE /W4 /permissive- /utf-8 /Zc:char8_t- /wd4100 /wd4201)
endif()

target_link_libraries(auto-dark PRIVATE
  d3d11 dxgi d3dcompiler dcomp dwmapi ws2_32 windowscodecs ole32 shcore winmm avrt)
