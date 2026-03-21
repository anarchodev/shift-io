cmake_minimum_required(VERSION 3.21)
project(shift-io VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(
    shift
    GIT_REPOSITORY https://github.com/anarchodev/shift.git
    GIT_TAG d7ccd8b
)
FetchContent_MakeAvailable(shift)

find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED liburing)

add_library(shift-io STATIC src/shift_io.c)

target_include_directories(shift-io
    PUBLIC
        include/
        ${shift_SOURCE_DIR}/include
    PRIVATE
        src/
        ${URING_INCLUDE_DIRS}
)

target_link_libraries(shift-io
    PUBLIC  shift
    PRIVATE ${URING_LINK_LIBRARIES}
)

# Sanitizer interface target (ASan + UBSan in Debug), matching shift's pattern
add_library(shift-io-sanitizers INTERFACE)
target_compile_options(shift-io-sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(shift-io-sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

option(SHIFT_IO_BUILD_TESTS "Build shift-io tests" OFF)

add_subdirectory(examples)
