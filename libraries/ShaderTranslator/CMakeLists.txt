cmake_minimum_required(VERSION 3.16)
project(ShaderTranslator VERSION 1.0.0 LANGUAGES CXX)

# C++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SHADER_TRANSLATOR_ENABLE_HLSL "Enable HLSL output translation support (SPIR-V → HLSL)" ON)
option(SHADER_TRANSLATOR_ENABLE_HLSL_INPUT "Enable HLSL input compilation support (HLSL → SPIR-V via DXC)" OFF)
option(SHADER_TRANSLATOR_USE_SYSTEM_DXC "Use system DXC from VulkanSDK instead of bundled" ON)

# ShaderTranslator source files
set(SHADER_TRANSLATOR_SOURCES
	src/spirv_translator.cpp
	src/shader_compiler.cpp
)

set(SHADER_TRANSLATOR_HEADERS
	include/shadertranslator/shader_translator.h
)

# Create static library
add_library(shader-translator STATIC
	${SHADER_TRANSLATOR_SOURCES}
	${SHADER_TRANSLATOR_HEADERS}
)

# Public include directory
target_include_directories(shader-translator PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ShaderTranslator is standalone - it only depends on compiled libraries, not ZVulkan's internals
# Add glslang and SPIRV-Cross as subdirectories
add_subdirectory(glslang EXCLUDE_FROM_ALL)
add_subdirectory(SPIRV-Cross EXCLUDE_FROM_ALL)

# Include directories for glslang and SPIRV-Cross
target_include_directories(shader-translator PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/glslang
	${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Cross
)

# Link against SPIRV-Cross libraries
target_link_libraries(shader-translator PUBLIC
	spirv-cross-core
	spirv-cross-glsl
	spirv-cross-msl
)

# Link against glslang for GLSL → SPIR-V compilation
target_link_libraries(shader-translator PUBLIC
	glslang-core
	SPIRV
	glslang-default-resource-limits
)

# Add HLSL output support if enabled
if(SHADER_TRANSLATOR_ENABLE_HLSL)
	target_link_libraries(shader-translator PUBLIC spirv-cross-hlsl)
	target_compile_definitions(shader-translator PUBLIC SHADER_TRANSLATOR_HLSL_ENABLED)
endif()

# Add HLSL input support if enabled (requires DXC)
if(SHADER_TRANSLATOR_ENABLE_HLSL_INPUT)
	if(SHADER_TRANSLATOR_USE_SYSTEM_DXC)
		# Try to find DXC from VulkanSDK
		find_path(DXC_INCLUDE_DIR dxc/dxcapi.h
			HINTS
				ENV VULKAN_SDK
			PATH_SUFFIXES include
		)

		find_library(DXC_LIBRARY
			NAMES dxcompiler libdxcompiler
			HINTS
				ENV VULKAN_SDK
			PATH_SUFFIXES lib
		)

		if(DXC_INCLUDE_DIR AND DXC_LIBRARY)
			message(STATUS "Found DXC: ${DXC_LIBRARY}")
			target_include_directories(shader-translator PRIVATE ${DXC_INCLUDE_DIR})
			target_link_libraries(shader-translator PRIVATE ${DXC_LIBRARY})
			target_compile_definitions(shader-translator PUBLIC SHADER_TRANSLATOR_HLSL_INPUT_ENABLED)
		else()
			message(WARNING "DXC not found in VulkanSDK - HLSL input support disabled")
		endif()
	else()
		# TODO: Support for bundled DXC
		message(WARNING "Bundled DXC not yet supported - use system DXC from VulkanSDK")
	endif()
endif()

# Compiler warnings
if(MSVC)
	target_compile_options(shader-translator PRIVATE /W4)
else()
	target_compile_options(shader-translator PRIVATE -Wall -Wextra)
endif()

# Installation rules (optional)
install(TARGETS shader-translator
	ARCHIVE DESTINATION lib
	LIBRARY DESTINATION lib
)

install(FILES ${SHADER_TRANSLATOR_HEADERS}
	DESTINATION include/shadertranslator
)
