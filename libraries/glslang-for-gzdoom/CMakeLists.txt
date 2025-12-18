cmake_minimum_required(VERSION 3.16)

project(glslang-for-gzdoom)

set(GLSLANG_SOURCES
	src/glslang/MachineIndependent/SymbolTable.h
	src/glslang/MachineIndependent/propagateNoContraction.cpp
	src/glslang/MachineIndependent/PoolAlloc.cpp
	src/glslang/MachineIndependent/Intermediate.cpp
	src/glslang/MachineIndependent/gl_types.h
	src/glslang/MachineIndependent/parseVersions.h
	src/glslang/MachineIndependent/attribute.cpp
	src/glslang/MachineIndependent/Scan.cpp
	src/glslang/MachineIndependent/iomapper.h
	src/glslang/MachineIndependent/ParseHelper.h
	src/glslang/MachineIndependent/glslang_tab.cpp.h
	src/glslang/MachineIndependent/SymbolTable.cpp
	src/glslang/MachineIndependent/RemoveTree.cpp
	src/glslang/MachineIndependent/Versions.h
	src/glslang/MachineIndependent/reflection.cpp
	src/glslang/MachineIndependent/LiveTraverser.h
	src/glslang/MachineIndependent/iomapper.cpp
	src/glslang/MachineIndependent/intermOut.cpp
	src/glslang/MachineIndependent/Versions.cpp
	src/glslang/MachineIndependent/Initialize.h
	src/glslang/MachineIndependent/linkValidate.cpp
	src/glslang/MachineIndependent/InfoSink.cpp
	src/glslang/MachineIndependent/Constant.cpp
	src/glslang/MachineIndependent/IntermTraverse.cpp
	src/glslang/MachineIndependent/propagateNoContraction.h
	src/glslang/MachineIndependent/glslang_tab.cpp
	src/glslang/MachineIndependent/ShaderLang.cpp
	src/glslang/MachineIndependent/preprocessor/Pp.cpp
	src/glslang/MachineIndependent/preprocessor/PpTokens.h
	src/glslang/MachineIndependent/preprocessor/PpAtom.cpp
	src/glslang/MachineIndependent/preprocessor/PpContext.cpp
	src/glslang/MachineIndependent/preprocessor/PpTokens.cpp
	src/glslang/MachineIndependent/preprocessor/PpScanner.cpp
	src/glslang/MachineIndependent/preprocessor/PpContext.h
	src/glslang/MachineIndependent/attribute.h
	src/glslang/MachineIndependent/localintermediate.h
	src/glslang/MachineIndependent/parseConst.cpp
	src/glslang/MachineIndependent/Initialize.cpp
	src/glslang/MachineIndependent/limits.cpp
	src/glslang/MachineIndependent/ParseContextBase.cpp
	src/glslang/MachineIndependent/RemoveTree.h
	src/glslang/MachineIndependent/ParseHelper.cpp
	src/glslang/MachineIndependent/Scan.h
	src/glslang/MachineIndependent/reflection.h
	src/glslang/MachineIndependent/ScanContext.h
	src/glslang/MachineIndependent/SpirvIntrinsics.cpp
	src/glslang/OSDependent/osinclude.h
	src/glslang/GenericCodeGen/Link.cpp
	src/glslang/GenericCodeGen/CodeGen.cpp
	src/glslang/Public/ShaderLang.h
	src/glslang/Include/ConstantUnion.h
	src/glslang/Include/InitializeGlobals.h
	src/glslang/Include/Common.h
	src/glslang/Include/PoolAlloc.h
	src/glslang/Include/arrays.h
	src/glslang/Include/ShHandle.h
	src/glslang/Include/InfoSink.h
	src/glslang/Include/ResourceLimits.h
	src/glslang/Include/Types.h
	src/glslang/Include/BaseTypes.h
	src/glslang/Include/intermediate.h
	src/glslang/Include/SpirvIntrinsics.h
	src/glslang/Include/build_info.h
	src/glslang/OSDependent/osinclude.h
	src/spirv/Logger.h
	src/spirv/GlslangToSpv.cpp
	src/spirv/SPVRemapper.h
	src/spirv/GLSL.ext.EXT.h
	src/spirv/hex_float.h
	src/spirv/doc.cpp
	src/spirv/disassemble.cpp
	src/spirv/SpvPostProcess.cpp
	src/spirv/bitutils.h
	src/spirv/InReadableOrder.cpp
	src/spirv/GLSL.ext.AMD.h
	src/spirv/GLSL.ext.NV.h
	src/spirv/SPVRemapper.cpp
	src/spirv/SpvBuilder.h
	src/spirv/GLSL.ext.KHR.h
	src/spirv/disassemble.h
	src/spirv/SpvBuilder.cpp
	src/spirv/GlslangToSpv.h
	src/spirv/doc.h
	src/spirv/SpvTools.cpp
	src/spirv/spvIR.h
	src/spirv/Logger.cpp
	src/spirv/SpvTools.h
	src/spirv/GLSL.std.450.h
	src/spirv/NonSemanticDebugPrintf.h
	src/OGLCompilersDLL/InitializeDll.cpp
	src/OGLCompilersDLL/InitializeDll.h
)

set(GLSLANG_WIN32_SOURCES
	src/glslang/OSDependent/Windows/ossource.cpp
)

set(GLSLANG_UNIX_SOURCES
	src/glslang/OSDependent/Unix/ossource.cpp
)

include_directories(src)

if(WIN32)
	list(APPEND GLSLANG_SOURCES ${GLSLANG_WIN32_SOURCES})
	add_definitions(-DUNICODE -D_UNICODE)
else()
	list(APPEND GLSLANG_SOURCES ${GLSLANG_UNIX_SOURCES})
	add_definitions(-DUNIX -D_UNIX)
	add_link_options(-pthread)
endif()

add_library(glslang-for-gzdoom STATIC ${GLSLANG_SOURCES})
set_target_properties(glslang-for-gzdoom PROPERTIES CXX_STANDARD 17)

if(MSVC)
	set_property(TARGET glslang-for-gzdoom PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
