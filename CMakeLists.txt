cmake_minimum_required(VERSION 3.16)

project(Z80Tools
        VERSION 0.1.0
        DESCRIPTION "Z80 assembler, disassembler and (planned) machine code monitor"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(Z80_BUILD_TESTS "Build the unit tests" ON)
option(Z80_BUILD_TOOLS "Build developer tools (cross-check helpers)" OFF)
option(Z80_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(Z80_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

function(z80_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(Z80_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(Z80_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(Z80_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# ---- core library ---------------------------------------------------------------------
add_library(z80core STATIC
  src/assembler.cpp
  src/decoder.cpp
  src/disassembler.cpp
  src/encoder.cpp
  src/expression.cpp
  src/format.cpp
  src/image_io.cpp)
target_include_directories(z80core PUBLIC include)
target_compile_features(z80core PUBLIC cxx_std_20)
z80_set_warnings(z80core)

# ---- command line tool ----------------------------------------------------------------
add_executable(z80tool app/main.cpp)
target_link_libraries(z80tool PRIVATE z80core)
z80_set_warnings(z80tool)

install(TARGETS z80tool RUNTIME DESTINATION bin)

# ---- developer tools ------------------------------------------------------------------
if(Z80_BUILD_TOOLS)
  add_executable(z80_dump_forms tools/dump_forms.cpp)
  target_link_libraries(z80_dump_forms PRIVATE z80core)
  z80_set_warnings(z80_dump_forms)
endif()

# ---- tests ----------------------------------------------------------------------------
if(Z80_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
