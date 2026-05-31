cmake_minimum_required(VERSION 3.16)
project(MallocChecker C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Allow overriding LLVM/Clang install prefix:
#   cmake -DLLVM_DIR=/path/to/lib/cmake/llvm -DClang_DIR=... ..
if(NOT LLVM_DIR)
  if(EXISTS "/opt/homebrew/opt/llvm/lib/cmake/llvm")
    set(LLVM_DIR "/opt/homebrew/opt/llvm/lib/cmake/llvm")
    set(Clang_DIR "/opt/homebrew/opt/llvm/lib/cmake/clang")
  elseif(EXISTS "/usr/local/opt/llvm/lib/cmake/llvm")
    set(LLVM_DIR "/usr/local/opt/llvm/lib/cmake/llvm")
    set(Clang_DIR "/usr/local/opt/llvm/lib/cmake/clang")
  endif()
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVM_DIR: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${Clang_DIR}")
include(AddLLVM)
include(AddClang)

include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Build as a loadable Clang plugin module.
add_llvm_library(MallocCheckerPlugin MODULE
    plugin/MallocChecker.cpp
    PLUGIN_TOOL clang
)

set(CLANG_LINK_CLANG_DYLIB ON)
clang_target_link_libraries(MallocCheckerPlugin PRIVATE)

if(APPLE)
  set_target_properties(MallocCheckerPlugin PROPERTIES
      PREFIX ""
      SUFFIX ".dylib"
  )
endif()

target_compile_options(MallocCheckerPlugin PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Test target
add_custom_target(check
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_tests.sh
            $<TARGET_FILE:MallocCheckerPlugin>
            ${CMAKE_C_COMPILER}
    DEPENDS MallocCheckerPlugin
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running malloc-checker test suite"
)
