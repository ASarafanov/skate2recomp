cmake_minimum_required(VERSION 3.25)
project(Skate2Setup VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The setup requires a 64-bit Windows toolchain.")
endif()

# One exe: the launcher. tools/package_setup.py appends the program files to
# a copy of it to make Skate2-PC-Setup.exe. It needs no game files, no SDK and
# no Visual C++ runtime (static CRT), so it runs as is under Wine / Proton.
add_executable(skate2_setup WIN32
  src/main.cpp
  src/disc.cpp
  src/gamepad.cpp
  src/install.cpp
  src/launch.cpp
  src/util.cpp
  src/setup.rc)
target_compile_features(skate2_setup PRIVATE cxx_std_20)
target_compile_definitions(skate2_setup PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(skate2_setup PRIVATE
  advapi32 comctl32 ole32 shell32 user32 uuid)
set_target_properties(skate2_setup PROPERTIES
  OUTPUT_NAME "Launch Skate 2"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_compile_options(skate2_setup PRIVATE /W4 /utf-8 /permissive-)
  # The manifest comes from setup.rc.
  target_link_options(skate2_setup PRIVATE /MANIFEST:NO)
endif()
