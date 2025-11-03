# Shader Translator Library

Cross-platform shader translation library for GZDoom. Translates SPIR-V bytecode to Metal Shading Language (MSL) and HLSL.

## Overview

This library provides a C++ API for translating SPIR-V shader bytecode (generated from GLSL by glslang) to platform-specific shader languages:

- **Metal Shading Language (MSL)** - for macOS Metal renderer (Metal 2.0+)
- **HLSL** - for future Windows D3D12 renderer (Shader Model 6.0+)

## Architecture

```
GLSL Shaders (wadsrc/static/shaders/glsl/*.fp, *.vp)
         │
         ▼
    glslang (in ZVulkan)
         │
         ▼
    SPIR-V bytecode
         │
         ▼
    SPIRV-Cross (this library)
         │
    ┌────┴────┐
    ▼         ▼
   MSL      HLSL
```

## Integration

The library reuses GZDoom's existing glslang integration from the Vulkan renderer, requiring only SPIRV-Cross as an additional dependency.

### Build Requirements

- CMake 3.10+
- C++17 compiler
- SPIRV-Cross (included as git submodule)

### Building

```bash
cd libraries/shader-translator
mkdir build && cd build
cmake ..
make
./examples/test_spirv_to_msl
```

## Usage Example

```cpp
#include <shadertranslator/shader_translator.h>

// Assuming you have SPIR-V bytecode from glslang
std::vector<uint32_t> spirv = CompileGLSLToSPIRV(glslSource);

// Create translator
ShaderTranslator::SPIRVTranslator translator;

// Translate to MSL (Metal 2.0)
auto result = translator.TranslateToMSL(spirv, 20);

if (result.success)
{
    // Use result.source as MSL shader code
    NSString* mslSource = [NSString stringWithUTF8String:result.source.c_str()];
    id<MTLLibrary> library = [device newLibraryWithSource:mslSource
                                                  options:nil
                                                    error:&error];
}
```

## Future Work

- Shader caching system
- Automatic binding remapping for Vulkan → Metal descriptor sets
- GLSL → SPIR-V convenience wrapper (currently in ZVulkan)
- Shader reflection API for uniform/attribute extraction

## License

Same license as GZDoom (3-clause BSD).
