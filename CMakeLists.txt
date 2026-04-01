cmake_minimum_required(VERSION 3.16)
project(ShaderTranslator VERSION 1.0.0 LANGUAGES CXX)

# C++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(SHADER_TRANSLATOR_ENABLE_HLSL "Enable HLSL output translation support (SPIR-V → HLSL)" ON)
option(SHADER_TRANSLATOR_ENABLE_HLSL_INPUT "Enable HLSL input compilation support (HLSL → SPIR-V via DXC)" OFF)
option(SHADER_TRANSLATOR_USE_SYSTEM_DXC "Use system DXC from VulkanSDK instead of bundled" ON)

# -------------------------------------------------------------------------
# glslang (submodule at libraries/ShaderTranslator/glslang, tag 11.6.0)
# Disable all tools, tests, and install rules — we only need the libraries.
# -------------------------------------------------------------------------
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST            OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL    ON  CACHE BOOL "" FORCE)
add_subdirectory(glslang EXCLUDE_FROM_ALL)

# ShaderTranslator source files (glslang sources are now built via its own CMake)
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

# glslang targets propagate their own include dirs ($<BUILD_INTERFACE:..>)
# but we still need the parent for "glslang/Public/ShaderLang.h" style includes.
target_include_directories(shader-translator PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/glslang  # for SPIRV/GlslangToSpv.h
	${CMAKE_CURRENT_SOURCE_DIR}          # for glslang/Public/ShaderLang.h
)

# -------------------------------------------------------------------------
# SPIRV-Cross (submodule at libraries/ShaderTranslator/SPIRV-Cross)
# -------------------------------------------------------------------------
add_subdirectory(SPIRV-Cross EXCLUDE_FROM_ALL)

target_include_directories(shader-translator PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Cross
)

# Link against glslang + SPIRV (OGLCompilersDLL is linked internally by glslang)
target_link_libraries(shader-translator PUBLIC
	glslang
	SPIRV
	spirv-cross-core
	spirv-cross-glsl
	spirv-cross-msl
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

# Compiler warnings (only for our own sources, not glslang/SPIRV-Cross)
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
