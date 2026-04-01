# ShaderTranslator

**Cross-platform shader translation library for GZDoom**, providing unified shader compilation from GLSL/HLSL to SPIR-V to Metal Shading Language (MSL) and HLSL.

## Overview

ShaderTranslator provides a complete shader compilation pipeline for modern graphics APIs:

**Supported Workflows:**
```
GLSL → SPIR-V → MSL (Metal)
              → HLSL (D3D12)

HLSL → SPIR-V → MSL (Metal)    [Optional: requires DXC]
              → HLSL (D3D12)
```

**Target Platforms:**
- **Metal (MSL)**: macOS/iOS Metal renderer
- **Direct3D 12 (HLSL)**: Windows D3D12 renderer (optional)

## Features

- **Unified API**: Compile from GLSL or HLSL to any target language
- **SPIR-V Caching**: Cache intermediate bytecode for faster re-compilation
- **Platform Optimizations**: Metal version targeting (2.0-2.4), HLSL shader models
- **Resource Reflection**: Automatic binding point translation
- **Shader Stages**: Vertex, Fragment, Compute shader support
- **Preprocessor**: Define macros for conditional compilation

## Building

ShaderTranslator is built as part of the GZDoom build system when Vulkan or Metal support is enabled.

### CMake Options

- `SHADER_TRANSLATOR_ENABLE_HLSL`: Enable HLSL output (SPIR-V → HLSL) (default: ON)
- `SHADER_TRANSLATOR_ENABLE_HLSL_INPUT`: Enable HLSL input (HLSL → SPIR-V via DXC) (default: OFF)
- `SHADER_TRANSLATOR_USE_SYSTEM_DXC`: Use system DXC from VulkanSDK instead of bundled (default: ON)

### Dependencies

**Required:**
- **glslang**: GLSL to SPIR-V compilation (from ZVulkan)
- **SPIRV-Cross**: SPIR-V to MSL/HLSL translation (from ZVulkan)
  - spirv-cross-core
  - spirv-cross-glsl
  - spirv-cross-msl

**Optional:**
- **spirv-cross-hlsl**: HLSL output support (enabled with `SHADER_TRANSLATOR_ENABLE_HLSL`)
- **DXC (DirectX Shader Compiler)**: HLSL input support (enabled with `SHADER_TRANSLATOR_ENABLE_HLSL_INPUT`)
  - Available in VulkanSDK 1.3.216+

## Usage

### Unified Shader Compiler (Recommended)

```cpp
#include <shadertranslator/shader_translator.h>

using namespace ShaderTranslator;

// Create unified compiler
ShaderCompiler compiler;

// Example 1: Compile GLSL vertex shader to MSL
std::string glslVertex = R"(
    #version 450
    layout(location = 0) in vec3 position;
    layout(location = 1) in vec2 texCoord;

    void main() {
        gl_Position = vec4(position, 1.0);
    }
)";

auto result = compiler.CompileGLSL(
    glslVertex,
    ShaderStage::Vertex,
    TargetLanguage::MSL,
    PlatformVersion{ .metalVersion = 22 }  // Metal 2.2
);

if (result.success) {
    std::string mslCode = result.source;
    // Use with MTLDevice newLibraryWithSource:
}

// Example 2: Compile GLSL to HLSL with defines
std::vector<std::string> defines = { "USE_TEXTURES", "SHADOW_QUALITY 2" };
auto hlslResult = compiler.CompileGLSL(
    glslVertex,
    ShaderStage::Vertex,
    TargetLanguage::HLSL,
    PlatformVersion{ .hlslShaderModel = 60 },
    defines
);

// Example 3: Cache SPIR-V for faster recompilation
compiler.CompileGLSL(glslVertex, ShaderStage::Vertex, TargetLanguage::SPIRV);
std::vector<uint32_t> cachedSPIRV = compiler.GetLastSPIRV();
// Save to disk, then later:
SPIRVTranslator translator;
auto fastResult = translator.TranslateToMSL(cachedSPIRV, 22);
```

### Low-Level SPIR-V Translation

```cpp
// If you already have SPIR-V bytecode
SPIRVTranslator translator;
std::vector<uint32_t> spirvBytecode = /* ... */;

auto result = translator.TranslateToMSL(spirvBytecode, 22);  // Metal 2.2
```

## Integration

ShaderTranslator is designed to be library-agnostic and can be used standalone or integrated into larger projects. It uses SPIRV-Cross and glslang from the ZVulkan library directory but is built as a separate library.

## License

Same as GZDoom (GPL with additional permissions - see parent LICENSE files)

## Architecture

```
ShaderTranslator/
├── include/shadertranslator/
│   └── shader_translator.h    # Public API
├── src/
│   └── spirv_translator.cpp   # SPIRV-Cross wrapper implementation
├── CMakeLists.txt             # Build configuration
└── README.md                  # This file
```

The library relies on glslang and SPIRV-Cross from `../ZVulkan/src/` but is built independently.

## Metal Version Support

| Metal Version | macOS Version | Features |
|--------------|---------------|----------|
| 2.0 | 10.13+ | Base Metal 2 support |
| 2.1 | 10.14+ | Additional features |
| 2.2 | 10.15+ | More features |
| 2.3 | 11.0+ | Advanced features |
| 2.4 | 12.0+ | Latest features |

## Contributing

When modifying ShaderTranslator:
1. Maintain compatibility with the public API in `shader_translator.h`
2. Keep the library focused on translation only
3. Test with both MSL and HLSL outputs (if HLSL enabled)
4. Update this README for any API changes
