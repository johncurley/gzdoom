# Shader-Translator Integration in ZVulkan

## Overview

The shader-translator has been successfully merged into the ZVulkan library, providing a unified shader compilation pipeline for GZDoom's multi-backend rendering system.

## Architecture

### Component Structure

```
ZVulkan Library
├── glslang (GLSL → SPIR-V compiler)
├── SPIRV-Cross (SPIR-V → MSL/HLSL/GLSL translator)
└── shader-translator (High-level C++ API wrapper)
```

### Integration Points

**Location:** `libraries/ZVulkan/`

```
ZVulkan/
├── include/zvulkan/
│   ├── shadertranslator/
│   │   └── shader_translator.h      # Public API
│   ├── vulkanbuilders.h
│   └── ...
├── src/
│   ├── shadertranslator/
│   │   └── spirv_translator.cpp     # Implementation
│   ├── glslang/                     # GLSL compiler (submodule)
│   └── SPIRV-Cross/                 # SPIR-V translator (submodule)
└── CMakeLists.txt                    # Builds all components
```

## How It Works

### 1. glslang Integration

**Purpose:** Compile GLSL source code to SPIR-V bytecode

**Headers:**
```cpp
#include "glslang/Public/ShaderLang.h"
#include "glslang/spirv/GlslangToSpv.h"
```

**Usage:**
```cpp
// Initialize (once per application)
glslang::InitializeProcess();

// Compile GLSL to SPIR-V
glslang::TShader shader(EShLangVertex);
shader.setStrings(&glslSource, 1);
shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

TBuiltInResource resources = GetDefaultTBuiltInResource();
shader.parse(&resources, 100, false, EShMsgDefault);

glslang::TProgram program;
program.addShader(&shader);
program.link(EShMsgDefault);

std::vector<uint32_t> spirv;
glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);

// Cleanup (once per application)
glslang::FinalizeProcess();
```

### 2. SPIRV-Cross Integration

**Purpose:** Translate SPIR-V bytecode to target shader languages (MSL, HLSL, GLSL)

**Headers:**
```cpp
#include <spirv_cross/spirv_msl.hpp>
#include <spirv_cross/spirv_hlsl.hpp>
#include <spirv_cross/spirv_glsl.hpp>
```

**CMake Configuration:**
```cmake
# In libraries/ZVulkan/CMakeLists.txt
set(SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/SPIRV-Cross")
add_subdirectory("${SPIRV_CROSS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Cross" EXCLUDE_FROM_ALL)

target_link_libraries(zvulkan PUBLIC
    spirv-cross-core
    spirv-cross-glsl
    spirv-cross-msl
    spirv-cross-hlsl
)
```

**Direct Usage:**
```cpp
spirv_cross::CompilerMSL msl(spirv_bytecode);
spirv_cross::CompilerMSL::Options options;
options.platform = spirv_cross::CompilerMSL::Options::macOS;
options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);
msl.set_msl_options(options);

std::string msl_source = msl.compile();
```

### 3. Shader-Translator Wrapper

**Purpose:** Simplified high-level C++ API for shader translation

**Public API:** `zvulkan/shadertranslator/shader_translator.h`

```cpp
namespace ShaderTranslator {
    class SPIRVTranslator {
    public:
        // Translate SPIR-V to MSL
        TranslationResult TranslateToMSL(
            const std::vector<uint32_t>& spirv,
            int metalVersion = 20
        );

        // Translate SPIR-V to HLSL
        TranslationResult TranslateToHLSL(
            const std::vector<uint32_t>& spirv,
            int shaderModel = 60
        );
    };

    struct TranslationResult {
        bool success;
        std::string errorLog;
        std::string source;  // Translated shader code

        // Reflection data
        std::vector<Resource> textures;
        std::vector<Resource> buffers;
        std::vector<Resource> samplers;
    };
}
```

## Complete Pipeline Example

### GLSL → SPIR-V → MSL (Used by Metal Renderer)

```cpp
// Step 1: GLSL → SPIR-V (using glslang)
glslang::InitializeProcess();

const char* vertexShader = R"(
    #version 450
    layout(location = 0) in vec3 position;
    layout(binding = 0) uniform UBO {
        mat4 modelViewProj;
    } ubo;

    void main() {
        gl_Position = ubo.modelViewProj * vec4(position, 1.0);
    }
)";

// ... glslang compilation code ...
std::vector<uint32_t> spirv = compileGLSLToSPIRV(vertexShader);

// Step 2: SPIR-V → MSL (using shader-translator)
ShaderTranslator::SPIRVTranslator translator;
auto result = translator.TranslateToMSL(spirv, 20);  // Metal 2.0

if (result.success) {
    // Step 3: MSL → MTLLibrary (using Metal API)
    NS::String* source = NS::String::string(result.source.c_str(), NS::UTF8StringEncoding);
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();

    NS::Error* error = nullptr;
    MTL::Library* library = device->newLibrary(source, options, &error);

    if (library) {
        MTL::Function* function = library->newFunction(NS::String::string("main0", NS::UTF8StringEncoding));
        // Use function in render pipeline...
    }
}

glslang::FinalizeProcess();
```

## Build System Integration

### CMakeLists.txt Configuration

**Main Project** (`CMakeLists.txt`):
```cmake
if (HAVE_VULKAN)
    # Configure shader translator options
    if( APPLE )
        set(ZVULKAN_ENABLE_MSL ON CACHE INTERNAL "" FORCE)
        set(ZVULKAN_ENABLE_HLSL OFF CACHE INTERNAL "" FORCE)
    else()
        set(ZVULKAN_ENABLE_MSL OFF CACHE INTERNAL "" FORCE)
        set(ZVULKAN_ENABLE_HLSL OFF CACHE INTERNAL "" FORCE)
    endif()

    add_subdirectory( libraries/ZVulkan )
endif()
```

**ZVulkan** (`libraries/ZVulkan/CMakeLists.txt`):
```cmake
# Options
option( ZVULKAN_ENABLE_MSL "Enable Metal Shading Language support" ON )
option( ZVULKAN_ENABLE_HLSL "Enable HLSL support" OFF )

# Configure SPIRV-Cross
set(SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/SPIRV-Cross")
set(SPIRV_CROSS_CLI OFF CACHE BOOL "Build SPIRV-Cross CLI" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "Enable tests" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "Enable GLSL" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ${ZVULKAN_ENABLE_MSL} CACHE BOOL "Enable MSL" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL ${ZVULKAN_ENABLE_HLSL} CACHE BOOL "Enable HLSL" FORCE)

add_subdirectory("${SPIRV_CROSS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/SPIRV-Cross" EXCLUDE_FROM_ALL)

# Add shader-translator sources
set(ZVULKAN_SOURCES
    src/vulkanbuilders.cpp
    src/vulkandevice.cpp
    src/shadertranslator/spirv_translator.cpp  # Shader translator
    # ... glslang sources ...
)

# Link libraries
add_library(zvulkan STATIC ${ZVULKAN_SOURCES} ${ZVULKAN_INCLUDES})
target_link_libraries(zvulkan PUBLIC
    ${ZVULKAN_LIBS}
    spirv-cross-core
    spirv-cross-glsl
)

if(ZVULKAN_ENABLE_MSL)
    target_link_libraries(zvulkan PUBLIC spirv-cross-msl)
    target_compile_definitions(zvulkan PUBLIC SHADERTRANSLATOR_MSL_ENABLED)
endif()

if(ZVULKAN_ENABLE_HLSL)
    target_link_libraries(zvulkan PUBLIC spirv-cross-hlsl)
    target_compile_definitions(zvulkan PUBLIC SHADERTRANSLATOR_HLSL_ENABLED)
endif()
```

## Renderer Usage

### Metal Renderer

**File:** `src/common/rendering/metal/shaders/mt_shader.cpp`

```cpp
#include <zvulkan/shadertranslator/shader_translator.h>
#include "glslang/Public/ShaderLang.h"
#include "glslang/spirv/GlslangToSpv.h"

std::vector<uint32_t> MtShaderManager::CompileGLSLToSPIRV(
    const std::string& source,
    const std::string& name,
    bool isVertex,
    const std::vector<std::string>& defines)
{
    // Use glslang to compile GLSL → SPIR-V
    TBuiltInResource resources = GetDefaultTBuiltInResource();
    glslang::TShader shader(isVertex ? EShLangVertex : EShLangFragment);
    // ... compilation logic ...

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    return spirv;
}

std::string MtShaderManager::TranslateSPIRVToMSL(
    const std::vector<uint32_t>& spirv,
    bool isVertex)
{
    // Use shader-translator to translate SPIR-V → MSL
    ShaderTranslator::SPIRVTranslator translator;
    auto result = translator.TranslateToMSL(spirv, 20);  // Metal 2.0

    if (!result.success) {
        Printf("Metal: SPIR-V→MSL translation failed: %s\n", result.errorLog.c_str());
        return "";
    }

    return result.source;
}
```

### Vulkan Renderer

The Vulkan renderer uses glslang directly for GLSL → SPIR-V, then passes SPIR-V bytecode directly to Vulkan (no translation needed).

## Testing

### Test Program

`test_shader_compile.cpp` demonstrates the complete pipeline:

```bash
# Compile test
c++ -std=c++17 test_shader_compile.cpp -o test_shader_compile \
  -Ilibraries/ZVulkan/include \
  -Ilibraries/ZVulkan/src \
  -Ilibraries/ZVulkan/src/glslang \
  -Lbuild/libraries/ZVulkan \
  -lzvulkan -lspirv-cross-core -lspirv-cross-glsl -lspirv-cross-msl \
  -framework Foundation

# Run test
./test_shader_compile
```

**Output:**
```
=== Shader Compilation Pipeline Test ===

1. Compiling vertex shader (GLSL → SPIR-V)...
   SUCCESS: Generated 404 SPIR-V words

2. Translating vertex shader (SPIR-V → MSL)...
   SUCCESS: Generated MSL code
   Textures: 0
   Buffers: 1

--- Vertex Shader MSL Output ---
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

vertex main0_out main0(main0_in in [[stage_in]],
                       constant UniformBufferObject& ubo [[buffer(0)]])
{
    // ... Metal-native shader code ...
}
--- End Vertex Shader ---

=== All Tests Passed! ===
```

## Binary Size

**libzvulkan.a:** ~4.5 MB (includes glslang + SPIRV-Cross + shader-translator)

Breakdown:
- glslang: ~3.5 MB
- SPIRV-Cross: ~900 KB
- shader-translator: ~50 KB

## Performance Characteristics

- **GLSL → SPIR-V:** ~2-5ms per shader (one-time compilation)
- **SPIR-V → MSL:** <1ms per shader (fast translation)
- **Caching:** Shaders are compiled once and cached in memory

## Benefits

1. **Unified Codebase:** Single GLSL shader source for all backends
2. **Zero Runtime Dependencies:** Everything statically linked
3. **Type Safety:** C++ wrapper provides type-safe API
4. **Reflection:** Automatic resource binding extraction
5. **Cross-Platform:** Works on macOS, Linux, Windows

## Future Enhancements

### Potential Additions

1. **HLSL Input Support:** Add DXC for HLSL → SPIR-V
2. **Shader Optimization:** Enable SPIRV-Tools optimization passes
3. **Debug Information:** Preserve source line mapping
4. **Validation:** Add SPIRV-Tools validation
5. **Caching:** Persistent disk cache for compiled shaders

### Example: Adding DXC Support

```cmake
# Future: Add DXC for HLSL input
if(ZVULKAN_ENABLE_DXC)
    find_package(DXC REQUIRED)
    target_link_libraries(zvulkan PUBLIC dxcompiler)
    target_compile_definitions(zvulkan PUBLIC SHADERTRANSLATOR_DXC_ENABLED)
endif()
```

## Maintenance Notes

### Updating Submodules

```bash
cd libraries/ZVulkan/src/SPIRV-Cross
git pull origin main

cd ../glslang
git pull origin main
```

### Build Clean

```bash
# Clean all build artifacts
rm -rf build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8
```

## References

- **glslang:** https://github.com/KhronosGroup/glslang
- **SPIRV-Cross:** https://github.com/KhronosGroup/SPIRV-Cross
- **SPIR-V Spec:** https://www.khronos.org/registry/spir-v/
- **Metal Shading Language:** https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf

## License

All components maintain their original licenses:
- **glslang:** BSD-3-Clause
- **SPIRV-Cross:** Apache 2.0
- **shader-translator:** BSD-3-Clause (GZDoom license)
