# ShaderTranslator Architecture

## Overview

ShaderTranslator provides a **layered compilation pipeline** that separates shader language parsing from target code generation. This allows optional, modular support for different input languages.

## Compilation Pipeline

```
┌─────────────────────────────────────────────────────────────┐
│                    Input Languages                          │
├─────────────────┬──────────────────┬────────────────────────┤
│   GLSL          │   HLSL           │   SPIR-V               │
│  (glslang)      │  (DXC/Optional)  │  (pre-compiled)        │
└────────┬────────┴────────┬─────────┴───────────┬────────────┘
         │                 │                     │
         └─────────────────┴─────────────────────┘
                           ▼
                    ┌──────────────┐
                    │   SPIR-V     │  ← Intermediate bytecode
                    │  (cached)    │
                    └──────┬───────┘
                           │
         ┌─────────────────┴─────────────────┐
         ▼                                   ▼
  ┌─────────────┐                    ┌─────────────┐
  │ SPIRV-Cross │                    │ SPIRV-Cross │
  │    MSL      │                    │    HLSL     │
  └─────────────┘                    └─────────────┘
         │                                   │
         ▼                                   ▼
    Metal API                           D3D12 API
```

## Layer Architecture

### Layer 1: Language Parsers (Optional/Required)

**GLSL Parser (Required - glslang)**
- **Library**: glslang from ZVulkan
- **Linking**: Static, always available
- **Usage**: Automatic for all GLSL shaders
- **Entry Point**: `ShaderCompiler::CompileGLSL()`

```cpp
// Always available - no conditional compilation needed
auto result = compiler.CompileGLSL(glslSource, ShaderStage::Vertex, TargetLanguage::MSL);
```

**HLSL Parser (Optional - DXC)**
- **Library**: DirectX Shader Compiler (dxcompiler.dylib/dll)
- **Linking**: Dynamic, loaded only when enabled
- **CMake Flag**: `SHADER_TRANSLATOR_ENABLE_HLSL_INPUT=ON`
- **Compile Define**: `SHADER_TRANSLATOR_HLSL_INPUT_ENABLED`
- **Entry Point**: `ShaderCompiler::CompileHLSL()`

```cpp
#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
auto result = compiler.CompileHLSL(hlslSource, ShaderStage::Vertex, TargetLanguage::MSL);
#endif
```

### Layer 2: SPIR-V Bytecode (Universal IR)

All language parsers produce **SPIR-V bytecode** as intermediate representation:
- Platform-agnostic
- Cacheable to disk
- Optimizable

```cpp
// Get SPIR-V for caching
compiler.CompileGLSL(source, stage, TargetLanguage::SPIRV);
std::vector<uint32_t> spirv = compiler.GetLastSPIRV();
// Save to disk...

// Later: translate cached SPIR-V
SPIRVTranslator translator;
auto mslResult = translator.TranslateToMSL(cachedSPIRV, 22);
```

### Layer 3: Target Code Generators (Required)

**Metal Shading Language (MSL) - Always enabled**
- **Library**: spirv-cross-msl
- **Target**: Metal 2.0-2.4 (macOS 10.13+)

**HLSL Output (Optional)**
- **Library**: spirv-cross-hlsl
- **CMake Flag**: `SHADER_TRANSLATOR_ENABLE_HLSL=ON` (default ON)
- **Target**: Shader Model 6.0+ (D3D12)

## Integration Strategies

### Strategy 1: Engine Runtime (GZDoom)

```cpp
// Metal renderer shader compilation
ShaderCompiler compiler;
auto result = compiler.CompileGLSL(
    glslShaderSource,
    ShaderStage::Vertex,
    TargetLanguage::MSL,
    PlatformVersion{ .metalVersion = 22 }
);

if (result.success) {
    MTL::Library* library = device->newLibrary(
        NS::String::string(result.source.c_str(), NS::UTF8StringEncoding),
        nullptr, &error
    );
}
```

### Strategy 2: Shader Asset Pipeline (Build-time)

```cpp
// Pre-compile shaders at build time and cache SPIR-V
ShaderCompiler compiler;

// Compile GLSL to SPIR-V
compiler.CompileGLSL(glslSource, stage, TargetLanguage::SPIRV);
std::vector<uint32_t> spirv = compiler.GetLastSPIRV();

// Save SPIR-V to .spv file
SaveToFile("shader.vert.spv", spirv);

// At runtime: Load and translate
std::vector<uint32_t> cachedSPIRV = LoadFromFile("shader.vert.spv");
SPIRVTranslator translator;
auto mslResult = translator.TranslateToMSL(cachedSPIRV, 22);
```

### Strategy 3: Cross-Platform Shader Tools

```cpp
// Convert HLSL to MSL (requires DXC)
#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
ShaderCompiler compiler;

// HLSL → SPIR-V → MSL
auto result = compiler.CompileHLSL(
    hlslSource,
    ShaderStage::Fragment,
    TargetLanguage::MSL
);

// Or: HLSL → SPIR-V → HLSL (for D3D12 compatibility layer)
auto d3d12Result = compiler.CompileHLSL(
    hlslSource,
    ShaderStage::Fragment,
    TargetLanguage::HLSL  // Re-emit as D3D12-compatible HLSL
);
#endif
```

## Optional DXC Integration

### When to Enable HLSL Input

**Enable if:**
- Building cross-platform shader tools
- Converting D3D12 shaders to Metal
- Porting Windows games to macOS
- Research/experimentation with shader languages

**Disable if:**
- Only using GLSL shaders (GZDoom default)
- Minimizing dependencies
- DXC not available on platform

### DXC Build Configuration

**macOS (VulkanSDK):**
```bash
cmake .. -DSHADER_TRANSLATOR_ENABLE_HLSL_INPUT=ON \
         -DSHADER_TRANSLATOR_USE_SYSTEM_DXC=ON

# DXC from: /path/to/VulkanSDK/x.x.x/macOS/lib/libdxcompiler.dylib
```

**Windows (VulkanSDK):**
```bash
cmake .. -DSHADER_TRANSLATOR_ENABLE_HLSL_INPUT=ON

# DXC from: C:\VulkanSDK\x.x.x\Lib\dxcompiler.lib
```

**Bundled DXC (future):**
```bash
cmake .. -DSHADER_TRANSLATOR_ENABLE_HLSL_INPUT=ON \
         -DSHADER_TRANSLATOR_USE_SYSTEM_DXC=OFF

# Uses libraries/ShaderTranslator/thirdparty/dxc/ (not yet implemented)
```

## Static vs Dynamic Linking

### glslang (GLSL → SPIR-V)

**Linkage**: Static
**Reason**: Core dependency from ZVulkan, always available
**Binary Impact**: ~500KB (already in GZDoom for Vulkan renderer)

### DXC (HLSL → SPIR-V)

**Linkage**: Dynamic (runtime loaded)
**Reason**: Optional feature, large library (~53MB)
**Binary Impact**:
- Disabled: 0 bytes (code removed by #ifdef)
- Enabled: ~5KB (DXC API wrapper code only)

**Runtime Loading:**
```cpp
#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
// DXC loaded via DxcCreateInstance() - only called when CompileHLSL() is used
// If DXC library not found, runtime error is thrown
#endif
```

### SPIRV-Cross (SPIR-V → MSL/HLSL)

**Linkage**: Static
**Reason**: Core translation engine
**Binary Impact**:
- MSL support: ~200KB (required for Metal)
- HLSL support: +50KB (optional, for D3D12 output)

## CMake Build Matrix

| Configuration | glslang | DXC | spirv-cross-msl | spirv-cross-hlsl | Use Case |
|--------------|---------|-----|-----------------|------------------|----------|
| **Minimal** (GZDoom default) | ✅ Static | ❌ | ✅ Static | ❌ | GLSL → MSL only |
| **Full Output** | ✅ Static | ❌ | ✅ Static | ✅ Static | GLSL → MSL/HLSL |
| **Full Input** | ✅ Static | ✅ Dynamic | ✅ Static | ✅ Static | GLSL/HLSL → MSL/HLSL |

## API Usage Examples

### Example 1: Simple GLSL → MSL (GZDoom)

```cpp
#include <shadertranslator/shader_translator.h>

ShaderTranslator::ShaderCompiler compiler;

std::string glsl = R"(
    #version 450
    layout(location = 0) in vec3 position;
    void main() {
        gl_Position = vec4(position, 1.0);
    }
)";

auto result = compiler.CompileGLSL(
    glsl,
    ShaderTranslator::ShaderStage::Vertex,
    ShaderTranslator::TargetLanguage::MSL,
    ShaderTranslator::PlatformVersion{ .metalVersion = 20 }
);

if (result.success) {
    // result.source contains MSL code ready for MTLDevice
    CreateMetalShader(result.source);
}
```

### Example 2: HLSL → MSL Conversion (with DXC)

```cpp
#ifdef SHADER_TRANSLATOR_HLSL_INPUT_ENABLED
std::string hlsl = R"(
    struct VS_INPUT {
        float3 position : POSITION;
    };

    float4 main(VS_INPUT input) : SV_POSITION {
        return float4(input.position, 1.0);
    }
)";

auto result = compiler.CompileHLSL(
    hlsl,
    ShaderTranslator::ShaderStage::Vertex,
    ShaderTranslator::TargetLanguage::MSL
);

if (result.success) {
    // HLSL translated to Metal - use on macOS
}
#endif
```

### Example 3: SPIR-V Caching for Performance

```cpp
// Build time: Compile once
ShaderCompiler compiler;
compiler.CompileGLSL(glslSource, stage, TargetLanguage::SPIRV);
std::vector<uint32_t> spirv = compiler.GetLastSPIRV();

// Save to asset file
SaveAssetFile("shader.vert.spv", spirv);

// Runtime: Fast translation from cached SPIR-V
std::vector<uint32_t> cachedSPIRV = LoadAssetFile("shader.vert.spv");
SPIRVTranslator translator;

// Platform-specific translation
#ifdef __APPLE__
    auto result = translator.TranslateToMSL(cachedSPIRV, 22);
#elif defined(_WIN32)
    auto result = translator.TranslateToHLSL(cachedSPIRV, 60);
#endif
```

## Binary Size Impact

### GZDoom Build (GLSL → MSL only)

```
libshader-translator.a:    53 KB
Dependencies:
  - glslang (from ZVulkan): ~500 KB (shared with Vulkan)
  - spirv-cross-msl:        ~200 KB
Total overhead:             ~753 KB
```

### With Full Features (GLSL/HLSL → MSL/HLSL)

```
libshader-translator.a:    58 KB (+5 KB for DXC wrapper)
Runtime dependencies:
  - libdxcompiler.dylib:    53 MB (not bundled, VulkanSDK)
  - spirv-cross-hlsl:       +50 KB
Total static overhead:      ~808 KB
```

## Performance Characteristics

| Operation | Time (macOS M1) | Cacheable |
|-----------|----------------|-----------|
| GLSL → SPIR-V (glslang) | ~5-20ms | ✅ Yes |
| HLSL → SPIR-V (DXC) | ~10-30ms | ✅ Yes |
| SPIR-V → MSL (SPIRV-Cross) | ~2-5ms | ❌ No (fast enough) |
| SPIR-V → HLSL (SPIRV-Cross) | ~2-5ms | ❌ No (fast enough) |

**Recommendation**: Cache SPIR-V bytecode for production, compile on-demand for development.

## Comparison to Other Engines

| Engine | GLSL Support | HLSL Support | SPIR-V Caching | Metal Backend |
|--------|--------------|--------------|----------------|---------------|
| **GZDoom (ShaderTranslator)** | ✅ glslang | ✅ DXC (opt) | ✅ Yes | ✅ MSL |
| Unreal Engine | ❌ HLSL only | ✅ Native | ✅ Yes | ✅ SPIRV-Cross |
| Unity | ✅ Limited | ✅ Native | ✅ Yes | ✅ SPIRV-Cross |
| id Tech 7 | ❌ | ✅ HLSL only | ✅ Yes | ✅ Custom |
| Source 2 | ✅ Via VPC | ❌ | ❌ No | ✅ Custom |

ShaderTranslator provides **best-of-both-worlds**: native GLSL support (for existing shaders) + optional HLSL (for cross-platform tools).
