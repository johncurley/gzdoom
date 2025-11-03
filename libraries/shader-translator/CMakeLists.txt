cmake_minimum_required(VERSION 3.10)
project(shader-translator VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SHADERTRANSLATOR_BUILD_EXAMPLES "Build example programs" ON)
option(SHADERTRANSLATOR_ENABLE_MSL "Enable Metal Shading Language support" ON)
option(SHADERTRANSLATOR_ENABLE_HLSL "Enable HLSL support" ON)

# SPIRV-Cross library (SPIR-V → MSL/HLSL/GLSL)
# Note: SPIRV-Cross is in ZVulkan/src/SPIRV-Cross
set(SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ZVulkan/src/SPIRV-Cross")

if(NOT EXISTS "${SPIRV_CROSS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SPIRV-Cross not found at ${SPIRV_CROSS_DIR}. Did you run 'git submodule update --init'?")
endif()

# Configure SPIRV-Cross build options
set(SPIRV_CROSS_CLI OFF CACHE BOOL "Build SPIRV-Cross CLI" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "Enable SPIRV-Cross tests" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "Enable GLSL support" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL ${SHADERTRANSLATOR_ENABLE_HLSL} CACHE BOOL "Enable HLSL support" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ${SHADERTRANSLATOR_ENABLE_MSL} CACHE BOOL "Enable MSL support" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "Enable C++ support" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "Enable reflection support" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "Enable C API" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "Enable utilities" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "Skip install" FORCE)

add_subdirectory("${SPIRV_CROSS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Cross" EXCLUDE_FROM_ALL)

# shader-translator library sources
set(SHADERTRANSLATOR_SOURCES
    src/spirv_translator.cpp
)

set(SHADERTRANSLATOR_HEADERS
    include/shadertranslator/shader_translator.h
)

# Create the library
add_library(shader-translator STATIC ${SHADERTRANSLATOR_SOURCES} ${SHADERTRANSLATOR_HEADERS})

# Include directories
target_include_directories(shader-translator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${SPIRV_CROSS_DIR}
)

# Link against SPIRV-Cross libraries
target_link_libraries(shader-translator
    PUBLIC
        spirv-cross-core
        spirv-cross-glsl
)

if(SHADERTRANSLATOR_ENABLE_MSL)
    target_link_libraries(shader-translator PUBLIC spirv-cross-msl)
    target_compile_definitions(shader-translator PUBLIC SHADERTRANSLATOR_MSL_ENABLED)
endif()

if(SHADERTRANSLATOR_ENABLE_HLSL)
    target_link_libraries(shader-translator PUBLIC spirv-cross-hlsl)
    target_compile_definitions(shader-translator PUBLIC SHADERTRANSLATOR_HLSL_ENABLED)
endif()

# Set position independent code for static library
set_target_properties(shader-translator PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Examples
if(SHADERTRANSLATOR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

message(STATUS "shader-translator library configured:")
message(STATUS "  MSL support: ${SHADERTRANSLATOR_ENABLE_MSL}")
message(STATUS "  HLSL support: ${SHADERTRANSLATOR_ENABLE_HLSL}")
message(STATUS "  Build examples: ${SHADERTRANSLATOR_BUILD_EXAMPLES}")
