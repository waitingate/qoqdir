cmake_minimum_required(VERSION 3.20)
project(Cubic LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wshadow)
  # One self-contained exe: no MinGW DLLs to ship alongside it.
  add_link_options(-static $<$<CONFIG:Release>:-s>)
endif()

set(WIN_LIBS ole32 oleaut32 shell32 shlwapi comctl32 uxtheme uuid)

# Path and folder logic, shared by the app and the tests.
add_library(cubic_core STATIC src/folder.cpp)
target_link_libraries(cubic_core PUBLIC ${WIN_LIBS})

add_executable(Cubic WIN32
  src/main.cpp src/app.cpp src/filelist.cpp src/tree.cpp src/shell.cpp src/selftest.cpp src/cubic.rc)
target_link_libraries(Cubic PRIVATE cubic_core)
if(NOT MSVC)
  target_link_options(Cubic PRIVATE -municode)
endif()

enable_testing()
add_executable(cubic_tests tests/tests.cpp)
target_link_libraries(cubic_tests PRIVATE cubic_core)
target_include_directories(cubic_tests PRIVATE src)
add_test(NAME cubic_tests COMMAND cubic_tests)
# Drives the real app in-process (opens a window briefly; never sends global keystrokes).
add_test(NAME cubic_selftest COMMAND Cubic --selftest)
