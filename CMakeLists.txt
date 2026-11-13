# Build Foyer on Windows with Visual Studio 2022 (Desktop C++ workload):
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(Foyer LANGUAGES CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed

file(GLOB FOYER_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Foyer WIN32 ${FOYER_SOURCES} res/foyer.rc)
target_compile_definitions(Foyer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(Foyer PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(Foyer PRIVATE /MANIFEST:NO)   # manifest comes from foyer.rc
endif()
target_link_libraries(Foyer PRIVATE d2d1 dwrite ole32 oleaut32 uuid dwmapi shcore powrprof wtsapi32 secur32 shell32 user32 gdi32 advapi32 shlwapi winhttp bcrypt comdlg32 version)
set_target_properties(Foyer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
