cmake_minimum_required(VERSION 3.21)
project(RawReader VERSION 0.1.0 LANGUAGES C)

if(NOT WIN32)
    message(FATAL_ERROR "This CMake configuration is intended for the Windows build.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET "gtk4>=4.24.1")

add_executable(rawreader
    src/main.c
    src/app.c
    src/device.c
    src/raw_reader.c
    src/hex_dump.c
)

target_include_directories(rawreader PRIVATE include)
target_compile_definitions(rawreader PRIVATE _WIN32_WINNT=0x0601)
target_compile_options(rawreader PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rawreader PRIVATE PkgConfig::GTK4 setupapi uuid)

install(TARGETS rawreader RUNTIME DESTINATION .)
