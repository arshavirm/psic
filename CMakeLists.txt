cmake_minimum_required(VERSION 3.20)
project(PSIC VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

find_package(LLVM REQUIRED CONFIG)
find_package(Threads REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

option(PSIC_BUILD_CLI "Build the psic command-line compiler" ON)
option(BUILD_SHARED_LIBS "Build PSIC as a shared library" OFF)
option(PSIC_SANITIZE "Enable address and undefined behavior sanitizers" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(psic_lib
    src/compiler.cpp src/target.cpp src/codegen.cpp src/validation.cpp
    src/lexer.cpp src/parser.cpp src/logging.cpp)
add_library(PSIC::psic ALIAS psic_lib)
set_target_properties(psic_lib PROPERTIES
    OUTPUT_NAME psic EXPORT_NAME psic POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_features(psic_lib PUBLIC cxx_std_17)

include(GenerateExportHeader)
generate_export_header(psic_lib BASE_NAME PSIC
    EXPORT_FILE_NAME ${PROJECT_BINARY_DIR}/include/psic/export.hpp)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(psic_lib PUBLIC PSIC_STATIC_DEFINE)
endif()
target_include_directories(psic_lib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(psic_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(PSIC_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(psic_lib PRIVATE ${PSIC_LLVM_DEFINITIONS})
target_link_libraries(psic_lib PRIVATE Threads::Threads)
if(TARGET LLVM)
    target_link_libraries(psic_lib PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(llvm_libs
        core irreader passes support target targetparser
        AllTargetsCodeGens AllTargetsAsmParsers AllTargetsDescs AllTargetsInfos)
    target_link_libraries(psic_lib PRIVATE ${llvm_libs})
endif()

if(MSVC)
    target_compile_options(psic_lib PRIVATE /W4)
else()
    target_compile_options(psic_lib PRIVATE -Wall -Wextra)
endif()
if(PSIC_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "PSIC_SANITIZE currently requires GCC or Clang")
    endif()
    target_compile_options(psic_lib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(psic_lib PUBLIC -fsanitize=address,undefined)
endif()

if(PSIC_BUILD_CLI)
    add_executable(psic src/main.cpp)
    target_link_libraries(psic PRIVATE PSIC::psic)
    # CLI logging is private; do not export internal library functions for it.
    target_sources(psic PRIVATE src/logging.cpp)
    if(BUILD_SHARED_LIBS AND UNIX)
        # Keep the CLI usable when the entire installation is relocated.
        file(RELATIVE_PATH PSIC_LIBRARY_FROM_BIN
            "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
        if(APPLE)
            set(PSIC_LOADER_PATH "@loader_path")
        else()
            set(PSIC_LOADER_PATH "$ORIGIN")
        endif()
        set_target_properties(psic PROPERTIES
            INSTALL_RPATH "${PSIC_LOADER_PATH}/${PSIC_LIBRARY_FROM_BIN}")
    endif()
endif()

include(CMakePackageConfigHelpers)
install(TARGETS psic_lib EXPORT PSICTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(PSIC_BUILD_CLI)
    install(TARGETS psic RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/psic DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${PROJECT_BINARY_DIR}/include/psic/export.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/psic)
configure_package_config_file(cmake/PSICConfig.cmake.in
    ${PROJECT_BINARY_DIR}/PSICConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PSIC)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/PSICConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT PSICTargets NAMESPACE PSIC:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PSIC)
install(FILES ${PROJECT_BINARY_DIR}/PSICConfig.cmake ${PROJECT_BINARY_DIR}/PSICConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PSIC)

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(PSIC_TOP_LEVEL ON)
    include(CTest)
else()
    set(PSIC_TOP_LEVEL OFF)
endif()
option(PSIC_BUILD_TESTS "Build PSIC compiler tests" ${PSIC_TOP_LEVEL})
if(BUILD_TESTING AND PSIC_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(psic_library_tests tests/library.cpp)
    target_link_libraries(psic_library_tests PRIVATE PSIC::psic Threads::Threads)
    add_test(NAME library COMMAND psic_library_tests)
    if(PSIC_BUILD_CLI)
        add_test(NAME architectures
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/architectures.py $<TARGET_FILE:psic>)
    endif()
endif()

if(BUILD_TESTING AND PSIC_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_executable(psic_emit_runtime tests/emit_runtime.cpp)
    target_link_libraries(psic_emit_runtime PRIVATE PSIC::psic)
    foreach(level O0 O2)
        set(runtime_object "${PROJECT_BINARY_DIR}/runtime-${level}${CMAKE_CXX_OUTPUT_EXTENSION}")
        add_custom_command(OUTPUT ${runtime_object}
            COMMAND psic_emit_runtime ${PROJECT_SOURCE_DIR}/tests/runtime.psi
                ${runtime_object} ${LLVM_HOST_TRIPLE} ${level}
            DEPENDS psic_emit_runtime ${PROJECT_SOURCE_DIR}/tests/runtime.psi
            VERBATIM)
        set_source_files_properties(${runtime_object} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        add_executable(psic_runtime_${level} tests/runtime.cpp ${runtime_object})
        add_test(NAME runtime_${level} COMMAND psic_runtime_${level})
    endforeach()
endif()

if(BUILD_TESTING AND PSIC_BUILD_TESTS)
    set_tests_properties(library PROPERTIES TIMEOUT 120)
    if(PSIC_BUILD_CLI)
        set_tests_properties(architectures PROPERTIES TIMEOUT 180)
        add_test(NAME cli COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/cli.py $<TARGET_FILE:psic>)
        set_tests_properties(cli PROPERTIES TIMEOUT 60)
    endif()
    if(NOT CMAKE_CROSSCOMPILING)
        set_tests_properties(runtime_O0 runtime_O2 PROPERTIES TIMEOUT 30)
        add_test(NAME installed_package COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/package.py
            --cmake ${CMAKE_COMMAND} --ctest ${CMAKE_CTEST_COMMAND}
            --build ${PROJECT_BINARY_DIR} --source ${PROJECT_SOURCE_DIR}/tests/package
            --config $<CONFIG> --llvm ${LLVM_DIR}
            --generator ${CMAKE_GENERATOR} --platform "${CMAKE_GENERATOR_PLATFORM}"
            --compiler ${CMAKE_CXX_COMPILER})
        set_tests_properties(installed_package PROPERTIES TIMEOUT 300)
    endif()
endif()
