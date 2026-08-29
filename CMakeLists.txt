cmake_minimum_required(VERSION 3.20)
project(PSIC)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM dir: ${LLVM_CMAKE_DIR}")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs -18)

file(GLOB_RECURSE SRC ${PROJECT_SOURCE_DIR}/src/*.cpp)

add_executable(psic ${SRC})

target_compile_options(psic PRIVATE
    $<$<CONFIG:Debug>:-g -O0 -Wall -Wextra>
    $<$<CONFIG:Release>:-O3 -DNDEBUG -march=native>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
)

target_link_libraries(psic PRIVATE ${llvm_libs})

option(PSIC_SANITIZE "Enable Sanitizers for debugging" OFF)
if(PSIC_SANITIZE)
    target_compile_options(psic PRIVATE -fsanitize=address,undefined)
    target_link_options(psic PRIVATE -fsanitize=address,undefined)
endif()
