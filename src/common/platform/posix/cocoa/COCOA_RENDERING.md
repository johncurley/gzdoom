# macOS Cocoa Rendering Architecture

## VSync Implementation

### Current Approach: NSOpenGLCPSwapInterval

GZDoom uses `NSOpenGLCPSwapInterval` for VSync control, which is the **correct approach for game engines**:

```objc
void SystemGLFrameBuffer::SetVSync(bool vsync)
{
    const GLint value = vsync ? 1 : 0;
    [[NSOpenGLContext currentContext] setValues:&value
                                   forParameter:NSOpenGLCPSwapInterval];
}
```

**How it works:**
- `vid_vsync = 0`: Buffer swap happens immediately (unlimited FPS)
- `vid_vsync = 1`: Buffer swap waits for vertical blank (locks to display refresh rate)
- Controlled by the `vid_vsync` CVAR
- Works with both fullscreen and windowed mode
- Compatible with OpenGL renderer

### CVDisplayLink: Why Not Used

**CVDisplayLink** is a display-synchronized callback mechanism designed for **GUI applications**, not game engines.

**Architectural Differences:**

| CVDisplayLink (GUI Apps) | GZDoom (Game Engine) |
|-------------------------|----------------------|
| Display drives rendering | Game loop drives rendering |
| Callback-based (event-driven) | Continuous loop (poll-based) |
| `OnWindowPaint()` called at vsync | `D_Display()` called every frame |
| Used by ZWidget | Uses NSOpenGLCPSwapInterval |

**Why NSOpenGLCPSwapInterval is better for GZDoom:**
1. **Game loop control**: GZDoom controls when frames are rendered
2. **Simpler threading**: No need for display link callback threads
3. **Better integration**: Works naturally with game timing code
4. **Standard approach**: Used by most OpenGL game engines
5. **Flexible**: Can disable VSync for benchmarking/uncapped FPS

**When CVDisplayLink makes sense:**
- GUI applications (like ZWidget)
- Apps where display drives the refresh
- Metal-based renderers with explicit presentation timing
- Apps that need display refresh rate information

### Future: Metal Renderer with CVDisplayLink

When implementing a native Metal renderer, CVDisplayLink could be beneficial:

```objc
// Metal presentation with CVDisplayLink
CVDisplayLinkSetOutputCallback(displayLink, &DisplayLinkCallback, self);

static CVReturn DisplayLinkCallback(...)
{
    // Get next drawable
    id<CAMetalDrawable> drawable = [metalLayer nextDrawable];

    // Render frame
    RenderFrame(drawable);

    // Present
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    return kCVReturnSuccess;
}
```

**Metal + CVDisplayLink benefits:**
- Precise display timing
- Better frame pacing
- Can use CAMetalLayer's display sync
- Integration with Metal 2+ features

## Current Renderer Support

### OpenGL Renderer (Modern)
- Uses `NSView` + manual `NSOpenGLContext` (not deprecated `NSOpenGLView`)
- VSync via `NSOpenGLCPSwapInterval`
- Swap buffers via `[context flushBuffer]`
- Compatible with macOS 10.13+

### Vulkan Renderer (via MoltenVK)
- Uses `NSView` + `CAMetalLayer`
- MoltenVK translates Vulkan to Metal
- VSync controlled by Vulkan presentation mode
- Metal 2 on macOS 10.13+

### Software Renderer (Poly)
- Uses OpenGL for presentation
- Renders to texture, then blits
- VSync via `NSOpenGLCPSwapInterval`

## Metal Renderer Framework (Future)

### Planned Architecture

```
GZDoom Metal Renderer
├── Device Management
│   ├── MTLDevice creation
│   ├── MTLCommandQueue
│   └── CAMetalLayer configuration
├── Shader Pipeline
│   ├── GLSL → SPIR-V (glslang)
│   ├── SPIR-V → MSL (SPIRV-Cross)
│   └── Shader caching
├── Resource Management
│   ├── MTLBuffer pools
│   ├── MTLTexture management
│   └── Descriptor sets
└── Frame Presentation
    ├── CVDisplayLink (optional)
    ├── CAMetalLayer drawable
    └── VSync control
```

### Shader Translation Pipeline

**Proposed library structure:**
```
libraries/shader-translator/
├── glslang/          # GLSL → SPIR-V
├── SPIRV-Cross/      # SPIR-V → MSL/HLSL
└── shader-cache/     # Compiled shader cache
```

**Workflow:**
1. GLSL shaders (existing)
2. Compile to SPIR-V (cross-platform IR)
3. Translate to MSL (Metal Shading Language)
4. Compile to MTLLibrary

**Benefits:**
- Share shader code with Vulkan renderer
- Cross-platform shader compilation
- Could also generate HLSL for future D3D12 renderer
- Efficient runtime shader loading

### Integration Points

**From Vulkan Renderer:**
- Command buffer structure
- Descriptor set management
- Pipeline state objects
- Resource barriers/transitions

**Metal-Specific:**
- MTLRenderPipelineState
- MTLDepthStencilState
- MTLCommandEncoder
- Argument buffers (Metal 2+)

### Native Handle Access

Already implemented in Phase 1:
```cpp
CocoaNativeHandle handle = framebuffer->GetNativeHandle();
handle.nsWindow;     // NSWindow*
handle.nsView;       // NSView*
handle.metalLayer;   // CAMetalLayer*
```

This provides everything needed for Metal renderer integration.

## Performance Characteristics

### VSync On (vid_vsync = 1)
- Frame rate locked to display refresh (60Hz, 120Hz, etc.)
- Tearing eliminated
- Input latency: 1 frame
- CPU/GPU can sleep between frames

### VSync Off (vid_vsync = 0)
- Uncapped frame rate
- Possible tearing
- Lower input latency
- Higher CPU/GPU usage
- Better for competitive gaming

### Frame Rate Cap (vid_maxfps)
- Independent of VSync
- Software-based frame limiting
- Can combine with VSync for frame pacing

## Recommendations

### For Current Development
- ✅ Keep NSOpenGLCPSwapInterval for VSync
- ✅ Don't add CVDisplayLink to OpenGL renderer
- ✅ Focus on Metal renderer framework instead

### For Metal Renderer (Future)
- ✅ Use CVDisplayLink for frame pacing
- ✅ Implement GLSL → SPIR-V → MSL pipeline
- ✅ Base on Vulkan renderer architecture
- ✅ Support Metal 2+ (macOS 10.13+)

## References

- [Apple: OpenGL Programming Guide](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/)
- [Apple: Metal Programming Guide](https://developer.apple.com/metal/)
- [SPIRV-Cross Documentation](https://github.com/KhronosGroup/SPIRV-Cross)
- [glslang Documentation](https://github.com/KhronosGroup/glslang)
