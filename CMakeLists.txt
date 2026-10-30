cmake_minimum_required(VERSION 3.20)
# The app's name. (In the code it is kAppName in src/util.h.)
project(qoqdir LANGUAGES CXX RC)

# docs/BUILDING.md has a g++ command that builds qoqdir.exe without CMake, from the same flags, files and libraries
# as below: change it with them.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# -O2, not CMake's -O3: as fast here (sorting and filtering are mostly Windows calls), and smaller.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

add_compile_definitions(UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
# Any warning fails the build, here and in the build on GitHub alike.
add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Werror)
# A bad index (or other misuse of the standard library) stops the program with a message
# instead of quietly reading the wrong memory.
add_compile_definitions(_GLIBCXX_ASSERTIONS)
# A signed integer overflow stops it too, instead of giving a wrong number (as in the Rust
# version). The crash log then shows exception 0xC000001D (an illegal instruction).
add_compile_options(-fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error)
# The runtime checks put the path of the failing header into the exe. Call the compiler's folder "mingw64" there,
# wherever it really is: the exe then holds no user name (it may live under C:\Users\<name>) and is the same from any PC.
get_filename_component(MINGW_ROOT "${CMAKE_CXX_COMPILER}" DIRECTORY)   # .../mingw64/bin
get_filename_component(MINGW_ROOT "${MINGW_ROOT}" DIRECTORY)            # .../mingw64
add_compile_options("-ffile-prefix-map=${MINGW_ROOT}=mingw64")
# One self-contained exe: no MinGW DLLs to ship alongside it. The linker drops the code and data
# nothing uses (most of libuuid's GUIDs, for one).
add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-static -Wl,--gc-sections $<$<CONFIG:Release>:-s>)

set(WIN_LIBS ole32 oleaut32 shell32 shlwapi comctl32 uuid)

# Path and folder logic, shared by the app and the tests.
add_library(core STATIC src/folder.cpp)
target_link_libraries(core PUBLIC ${WIN_LIBS})

# The app, compiled once and linked twice: into qoqdir.exe, what users get, and into qoqdir-test.exe, the
# same plus the self-test. So the self-test checks the very code that ships.
add_library(app OBJECT src/crashlog.cpp src/app.cpp src/filelist.cpp src/tree.cpp src/shell.cpp)
target_link_libraries(app PUBLIC core)

# windres reports no dependencies, so name what qoqdir.rc pulls in: a new version, manifest or icon then rebuilds it.
set_source_files_properties(res/qoqdir.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h;${CMAKE_CURRENT_SOURCE_DIR}/res/qoqdir.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/qoqdir.ico")

add_executable(${PROJECT_NAME} WIN32 src/main.cpp res/qoqdir.rc)
target_link_libraries(${PROJECT_NAME} PRIVATE app)
target_link_options(${PROJECT_NAME} PRIVATE -municode)

add_executable(${PROJECT_NAME}-test WIN32 src/main.cpp src/selftest.cpp res/qoqdir.rc)
target_compile_definitions(${PROJECT_NAME}-test PRIVATE QOQDIR_SELFTEST)
target_link_libraries(${PROJECT_NAME}-test PRIVATE app oleacc)
target_link_options(${PROJECT_NAME}-test PRIVATE -municode)

enable_testing()
add_executable(unit_tests tests/unit-tests.cpp src/crashlog.cpp)
target_link_libraries(unit_tests PRIVATE core)
target_include_directories(unit_tests PRIVATE src)
add_test(NAME unit_tests COMMAND unit_tests)
# Drives the real app in-process (opens a window briefly; never sends global keystrokes).
add_test(NAME selftest COMMAND ${PROJECT_NAME}-test --selftest)
