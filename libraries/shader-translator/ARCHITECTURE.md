# Shader Translation Architecture

## Overview

The shader-translator library enables GZDoom's GLSL shaders to run on Metal (macOS) and D3D12 (Windows) by translating the existing SPIR-V bytecode to platform-specific shader languages.

## Design Philosophy

**Reuse, Don't Rebuild**: This library leverages GZDoom's existing Vulkan renderer infrastructure:

1. **glslang** (already in ZVulkan) - Compiles GLSL → SPIR-V
2. **SPIRV-Cross** (new) - Translates SPIR-V → MSL/HLSL
3. **Existing shaders** - No shader rewrites needed!

## Translation Pipeline

```
┌──────────────────────────────────────────────────────────┐
│  GLSL Shaders (49 shaders in wadsrc/static/shaders/)    │
│  • Vertex shaders: main.vp, screenquad.vp                │
│  • Fragment shaders: Material, lighting, post-processing │
└──────────────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────┐
│  Preprocessing (VkShaderManager::LoadFragShader)         │
│  • Add #version directive (450/460)                      │
│  • Add descriptor set bindings                           │
│  • Add platform defines                                  │
│  • Include material functions                            │
│  • Include lighting functions                            │
└──────────────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────┐
│  glslang Compilation (in ZVulkan)                        │
│  • Parse GLSL                                            │
│  • Validate syntax                                       │
│  • Compile to SPIR-V                                     │
│  • Optimize (size/performance)                           │
└──────────────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────┐
│  SPIR-V Bytecode (std::vector<uint32_t>)                 │
│  • Platform-independent intermediate representation      │
│  • Used by Vulkan renderer directly                      │
│  • Input for shader-translator                           │
└──────────────────────────────────────────────────────────┘
                        │
        ┌───────────────┴───────────────┬──────────────┐
        │                               │              │
        ▼                               ▼              ▼
┌─────────────────┐       ┌──────────────────┐  ┌───────────┐
│  Vulkan Path    │       │  Metal Path      │  │ D3D12 Path│
│  (existing)     │       │  (macOS)         │  │ (future)  │
│                 │       │                  │  │           │
│  VkShaderModule │       │  SPIRV-Cross MSL │  │ SPIRV-    │
│  Direct use     │       │  → MTLLibrary    │  │ Cross HLSL│
└─────────────────┘       └──────────────────┘  └───────────┘
```

## Code Flow Example

### Vulkan Renderer (Existing)

```cpp
// In VkShaderManager::LoadFragShader()
FString glslCode = GetTargetGlslVersion();  // "#version 450 core"
glslCode << defines;                         // Material-specific defines
glslCode << shaderBindings;                  // Vulkan descriptor sets
glslCode << LoadPrivateShaderLump("main.fp");
glslCode << LoadPublicShaderLump("material.fp");

// Compile with glslang (in ShaderBuilder::Create)
glslang::TShader shader(EShLangFragment);
shader.parse(...);
glslang::GlslangToSpv(*intermediate, spirv, ...);

// Create Vulkan shader module
VkShaderModuleCreateInfo createInfo = {};
createInfo.pCode = spirv.data();
vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
```

### Metal Renderer (Future)

```cpp
// In MtShaderManager::LoadFragShader()
// Step 1: Reuse Vulkan's preprocessing (90% identical!)
FString glslCode = GetTargetGlslVersion();
glslCode << defines;
glslCode << metalBindings;  // Metal-specific bindings (similar to Vulkan)
glslCode << LoadPrivateShaderLump("main.fp");
glslCode << LoadPublicShaderLump("material.fp");

// Step 2: Compile GLSL → SPIR-V (reuse glslang like Vulkan)
std::vector<uint32_t> spirv = CompileGLSLToSPIRV(glslCode);

// Step 3: Translate SPIR-V → MSL (NEW: use shader-translator)
ShaderTranslator::SPIRVTranslator translator;
auto result = translator.TranslateToMSL(spirv, 20);  // Metal 2.0

// Step 4: Compile MSL → MTLLibrary
NSString* mslSource = [NSString stringWithUTF8String:result.source.c_str()];
id<MTLLibrary> library = [device newLibraryWithSource:mslSource
                                              options:nil
                                                error:&error];
```

## Binding Translation

### Vulkan Descriptor Sets

```glsl
// Vulkan GLSL (current)
layout(set = 0, binding = 0) uniform sampler2D ShadowMap;
layout(set = 1, binding = 0) uniform ViewpointUBO { ... };
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PushConstants { ... };
```

### Metal Argument Buffers

```metal
// Metal MSL (after SPIRV-Cross translation)
struct ViewpointUBO { ... };
struct PushConstants { ... };

fragment float4 main0(
    texture2d<float> ShadowMap [[texture(0)]],
    constant ViewpointUBO& viewpoint [[buffer(0)]],
    constant PushConstants& pushConstants [[buffer(1)]],
    texture2d<float> tex [[texture(1)]]
)
```

**SPIRV-Cross automatically handles this translation!**

## Resource Reflection

The library extracts binding information for the Metal renderer:

```cpp
TranslationResult result = translator.TranslateToMSL(spirv);

// Reflection data available
for (const auto& texture : result.textures)
{
    // Vulkan: set=0, binding=0
    // Metal: [[texture(0)]]
    printf("%s: Vulkan(set=%d, binding=%d) → Metal(texture=%d)\n",
           texture.name.c_str(), texture.set, texture.binding,
           texture.mslBinding);
}
```

## Performance Considerations

### Compilation Time

- **GLSL → SPIR-V**: ~10-50ms per shader (glslang)
- **SPIR-V → MSL**: ~1-5ms per shader (SPIRV-Cross)
- **Total**: ~11-55ms per shader

### Caching Strategy (Future)

```
~/.cache/gzdoom/metal_shaders/
├── main_fp_material_normal_abc123.metal  # Cached MSL source
├── main_fp_material_pbr_def456.metal
└── ...
```

Cache key: MD5(SPIR-V bytecode + Metal version)

## Integration Points

### Current Vulkan Renderer

```
src/common/rendering/vulkan/
├── shaders/
│   ├── vk_shader.h         ← VkShaderManager
│   └── vk_shader.cpp       ← LoadVertShader/LoadFragShader
└── system/
    └── vk_renderdevice.h   ← VulkanRenderDevice
```

### Future Metal Renderer

```
src/common/rendering/metal/
├── shaders/
│   ├── mt_shader.h         ← MtShaderManager (similar to Vulkan)
│   └── mt_shader.cpp       ← LoadVertShader/LoadFragShader + SPIRV-Cross
└── system/
    └── mt_renderdevice.h   ← MetalRenderDevice
```

## Dependencies

- **glslang** (existing in ZVulkan)
  - GLSL parser and validator
  - SPIR-V code generator
  - Already integrated, production-tested

- **SPIRV-Cross** (new in ZVulkan/src/SPIRV-Cross)
  - SPIR-V reflection
  - MSL code generator
  - HLSL code generator
  - Maintained by Khronos Group

## Testing Strategy

1. **Unit Tests**: Translate simple shaders, verify MSL compiles
2. **Integration Tests**: Translate all 49 GZDoom shaders
3. **Runtime Tests**: Render test scene with Metal renderer

## Future Enhancements

1. **Shader Variants**: Pre-compile common permutations
2. **Aggressive Caching**: Persist compiled MTLLibrary binaries
3. **Hot Reload**: Recompile shaders on-the-fly for debugging
4. **D3D12 Support**: Enable HLSL translation for Windows

## References

- [glslang GitHub](https://github.com/KhronosGroup/glslang)
- [SPIRV-Cross GitHub](https://github.com/KhronosGroup/SPIRV-Cross)
- [Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf)
- [SPIR-V Specification](https://www.khronos.org/registry/spir-v/)
