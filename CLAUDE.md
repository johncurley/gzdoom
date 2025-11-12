# GZDoom Codebase Architecture Guide

## 1. Build System

### Configuration & Building

**CMake Configuration (C++17)**
- Top-level: `/CMakeLists.txt`
- Key cmake version: 3.16+ required
- Standard: C++17 (mandatory), no compiler extensions

**Basic Build Commands**
```bash
# Configure build directory
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Compile (parallel)
cmake --build . -j$(nproc)

# Install
cmake --install .
```

**Platform-Specific Options**

*macOS:*
```bash
-D OSX_COCOA_BACKEND=ON  # Native Cocoa backend (default ON)
                         # If OFF, falls back to SDL2
```

*Unix/Linux:*
- Requires SDL2
- Optional: GTK+ for native dialogs

*Windows:*
- Direct3D input support built-in
- SDL2 for non-Windows backends

### Key Build Variables

**Renderer Backend Selection:**
```
vid_preferbackend = 0  # OpenGL (default)
vid_preferbackend = 1  # Vulkan (requires HAVE_VULKAN)
vid_preferbackend = 2  # OpenGL ES 2.0 (requires HAVE_GLES2)
```

**Feature Flags:**
```
ZDOOM_ENABLE_SWR=ON     # Enable software renderer (default ON)
NO_SWRENDERER=OFF       # Disable software renderer
NO_OPENAL=OFF           # Disable audio
HAVE_GLES2=ON/OFF       # Mobile GL support
HAVE_VULKAN=ON/OFF      # Vulkan support
```

### Tests & Validation

**No formal test suite present.** Testing relies on:
- Manual gameplay testing with mods
- CI/CD via GitHub Actions (see `.github/workflows/`)
- Platform-specific shader compilation
- Memory validation for save/load

---

## 2. High-Level Architecture

### Main Components & Interactions

**Game Loop Flow:**
```
D_DoomMain()
  ├─ InitResources()  [engine startup]
  ├─ LoadGameData()   [wadfiles, configs]
  └─ D_DoomLoop()     [main loop]
       └─ Frame tick (35 FPS game tick, ~60 FPS render):
           ├─ G_Ticker()           [game logic update]
           │   └─ DThinker iteration over all actors
           ├─ D_ProcessEvents()    [input handling]
           │   └─ G_Responder()    [game-specific input]
           ├─ D_Display()          [render frame]
           │   ├─ ViewRender()     [3D scene setup]
           │   ├─ Screen render    [scene → framebuffer]
           │   └─ 2D UI overlay    [HUD, menus]
           └─ Screen update         [present to display]
```

### Core Subsystems

**1. Rendering Pipeline** (`/src/common/rendering/`)
- **Abstract Layer:** `DFrameBuffer` (v_video.h) - platform-agnostic interface
- **Backends:** Three parallel implementations
  - OpenGL (`/gl/`) - Fixed-function + shaders
  - Vulkan (`/vulkan/`) - Modern API with explicit GPU management
  - Metal (`/metal/`) - Apple Silicon native (new)
- **Shared:** Hardware renderer abstraction (`/hwrenderer/`)

**2. Game Logic** (`/src/playsim/`)
- **Actor System:** `AActor` class (base for all game objects)
  - Inherits from `DThinker` - callback-based tick system
  - State machine-driven via `FState` definitions
  - Physics: collision, movement, gravity
- **Thinker Lists:** Organized by priority (statnum 0-127)
- **Level Data:** `FLevelLocals` - map state, lighting, sectors
- **Physics:** Full 3D collision detection and response

**3. Scripting System** (`/src/scripting/`)
- **ZScript:** Modern object-oriented scripting language
  - Compiled to bytecode (via `codegen.cpp`)
  - JIT compilation optional (`vm_jit` cvar)
  - Interop with C++ via `vmthunks.cpp`
- **DECORATE:** Legacy actor definition format (still supported)
- **ACS:** Scriptable level actions (classic Doom mapper scripting)

**4. Audio System** (`/src/common/audio/`)
- **ZMusic Library** - wrapper over multiple backends:
  - MIDI: FluidSynth, OPL, ADL
  - Tracker: libxmp, DUMB
  - Sample: OpenAL
- **Sound:** Dynamic 3D sound positioning
- **Music:** IWAD bank selection + custom compositions

**5. Platform Abstraction** (`/src/common/platform/`)
- **POSIX:** macOS (Cocoa + SDL2), Linux (SDL2)
- **Windows:** Native Win32 backend
- **Input:** Keyboard, mouse, joystick via SDL2/native
- **Video:** Window management, display mode selection

### Key Directories Structure

```
src/
├── common/
│   ├── rendering/       # GPU abstraction & backends
│   │   ├── gl/          # OpenGL implementation
│   │   ├── gles/        # OpenGL ES 2.0
│   │   ├── vulkan/      # Vulkan with full manager hierarchy
│   │   ├── metal/       # NEW: Apple Metal (metal-cpp)
│   │   ├── hwrenderer/  # Shared hardware renderer logic
│   │   │   ├── postprocessing/  # Bloom, FXAA, tonemapping
│   │   │   └── data/    # Vertex buffers, sky dome, light
│   │   └── v_video.h    # DFrameBuffer - abstract framebuffer
│   ├── engine/          # Core utilities (events, serialization)
│   ├── scripting/       # ZScript VM & compiler
│   ├── audio/           # Sound & music via ZMusic
│   ├── textures/        # Texture managers, materials
│   └── platform/        # OS-specific implementations
├── playsim/             # Game simulation (actors, physics, AI)
├── rendering/           # Legacy per-platform renderers (deprecated)
├── scripting/           # ThingDef, legacy scripting
└── [game-specific]/     # HUD, menus, maps, etc.

libraries/
├── ZVulkan/             # Vulkan C++ wrapper (SPIRV-Cross, glslang)
├── ZMusic/              # Audio engine (thirdparty backends)
└── metal-cpp/           # Metal C++ bindings (zero-overhead)
```

---

## 3. Rendering Architecture

### Backend Organization & Abstraction

**Platform Abstraction Hierarchy:**

```
DFrameBuffer (v_video.h) - Abstract interface
    ↓
SystemBaseFrameBuffer (gl_sysfb.h) - Platform-specific base
    ↓ (Backend-specific implementations)
    ├── OpenGLFrameBuffer (gl/)
    ├── VulkanRenderDevice (vulkan/system/)
    └── MetalRenderDevice (metal/system/)
```

**DFrameBuffer Interface** - Core capabilities exposed to engine:
```cpp
class DFrameBuffer {
    // Window management
    virtual bool IsFullscreen() = 0;
    virtual int GetClientWidth() = 0;
    virtual int GetClientHeight() = 0;
    
    // Resource creation (factory pattern)
    virtual IHardwareTexture* CreateHardwareTexture(int channels) { }
    virtual IVertexBuffer* CreateVertexBuffer() { }
    virtual IIndexBuffer* CreateIndexBuffer() { }
    
    // Render state
    virtual FRenderState* RenderState() { }
    
    // Frame lifecycle
    virtual void Update() { }        // Present frame
    virtual void BeginFrame() { }
    
    // Post-processing
    virtual void PostProcessScene(bool swscene, int fixedcm, float flash, ...) { }
    virtual void BlurScene(float amount) { }
    
    // Identification (used by engine for conditionals)
    virtual bool IsVulkan() { return false; }  // Vulkan only
    virtual int Backend() { }  // 0=GL, 1=Vk, 2=Metal
};
```

### Renderer Selection & Initialization

**Selection Logic** (v_video.cpp):
```cpp
int V_GetBackend() {
    int v = vid_preferbackend;
    // vid_preferbackend: 0=OpenGL, 1=Vulkan, 2=GLES2
    if (v == 3) v = 2;  // Fallback GLES2
    else if (v < 0 || v > 3) v = 0;  // Default OpenGL
    return v;
}
```

**Initialization Flow** (d_main.cpp → V_Init2):
1. `V_GetBackend()` determines active renderer
2. `CreateFrameBuffer()` instantiates backend-specific device
3. `screen->InitializeState()` runs backend initialization
4. Shader compilation occurs asynchronously during gameplay

### Vulkan Architecture (Most Modern)

**Render Device** (`vulkan/system/vk_renderdevice.h`):
```cpp
class VulkanRenderDevice : public SystemBaseFrameBuffer {
    std::shared_ptr<VulkanDevice> device;  // Low-level GPU handle
    
    // Manager accessors (factory pattern)
    VkCommandBufferManager* GetCommands();
    VkShaderManager* GetShaderManager();
    VkSamplerManager* GetSamplerManager();
    VkTextureManager* GetTextureManager();
    VkRenderPassManager* GetRenderPassManager();
    VkRenderState* GetRenderState();        // State machine
    VkPostprocess* GetPostprocess();
};
```

**Manager Hierarchy** (Ownership model):
```
VulkanRenderDevice
├── VkCommandBufferManager      # GPU work submission + synchronization
├── VkRenderPassManager         # Pipeline configuration & caching
├── VkRenderState               # State machine & draw dispatcher
├── VkShaderManager             # Shader compilation & caching
├── VkTextureManager            # Texture resource lifecycle
├── VkBufferManager             # Vertex/index/data buffers
├── VkSamplerManager            # Sampler object caching
├── VkDescriptorSetManager      # Resource binding
├── VkRenderBuffers (screen)    # Color/depth/normal targets
├── VkRenderBuffers (save)      # Screenshot buffer
└── VkPostprocess               # Bloom, tone mapping, FXAA
```

**RenderState State Machine** (`vulkan/renderer/vk_renderstate.h`):
```cpp
class VkRenderState : public FRenderState {
    // State tracking
    bool mDepthTest, mDepthWrite;
    int mDepthFunc;
    int mStencilRef, mStencilOp;
    int mColorMask, mCullMode;
    IntRect mScissor, mViewport;
    FRenderStyle mBlendMode;
    
    // Pipeline management
    VkPipelineKey mPipelineKey;      // Uniquely identifies pipeline
    VkRenderPassSetup* mPassSetup;
    
    // Draw dispatch
    void Draw(int primtype, int index, int count, bool apply = true);
    void DrawIndexed(int primtype, int index, int count, bool apply = true);
    
    // State application (lazy evaluation)
    void Apply(int primtype) {
        ApplyRenderPass(primtype);
        ApplyScissor();
        ApplyViewport();
        ApplyPipelineState();
        ApplyMaterial();  // Texture bindings
        ApplyBufferSets(); // Uniform buffers
    }
};
```

**Command Buffer Pattern** (`vulkan/system/vk_commandbuffer.h`):
- Dual-track: transfer (uploads) + graphics (draws)
- Ring-buffer with 8 concurrent submits
- Fence-based GPU synchronization
- Deferred deletion: resources freed after GPU consume

### Metal Architecture (Native Apple Silicon)

**Render Device** (`metal/system/mt_renderdevice.h`):
```cpp
class MetalRenderDevice : public SystemBaseFrameBuffer {
    std::shared_ptr<MetalDevice> device;  // MTL::Device + MTL::CommandQueue
    
    // Similar manager pattern to Vulkan
    MtCommandBufferManager* GetCommands();
    MtRenderState* GetRenderState();
    MtShaderManager* GetShaderManager();
    MtTextureManager* GetTextureManager();
    // ... etc
    
    int Backend() override { return 2; }  // Metal identifier
    bool IsMetal() override { return true; }
};
```

**Key Difference:** Uses `metal-cpp` (zero-overhead C++ bindings) instead of raw Metal API
- Wrapper-less abstraction → no performance penalty
- Type-safe command buffer construction
- Automatic reference counting

### OpenGL Architecture (Compatibility)

**Renderer** (`gl/gl_renderer.h`):
```cpp
class FGLRenderer {
    OpenGLFrameBuffer* framebuffer;
    FShaderManager* mShaderManager;
    FGLRenderBuffers* mBuffers;      // Screen + auxiliary buffers
    
    // Stereo & post-processing
    FPresentShader* mPresentShader;
    void PostProcessScene(int fixedcm, float flash, ...);
};
```

**State Management:** Uses OpenGL state machine directly (not cached)
**Shader System:** GLSL compilation + caching
**Compatibility:** Supports both legacy (1.3) and modern (4.6) GL

### Hardware Renderer Abstraction (`hwrenderer/`)

**Shared Rendering Logic** (backend-agnostic):
```cpp
// Mesh/geometry
class FFlatVertexBuffer      # 2D/flat geometry
class FSkyVertexBuffer       # Sky dome
class HWViewpointBuffer      # Camera matrices

// Lighting
class FLightBuffer           # Dynamic light data
class IShadowMap             # Shadow maps for dynamic lights

// Scene data
class HWDrawInfo             # Per-frame draw commands
struct HWPortal              # Portal rendering state

// Materials
class FMaterial              # Texture properties
class FGameTexture           # Game texture abstraction
```

**Post-Processing Pipeline** (`hwrenderer/postprocessing/`):
```cpp
class HWPostProcess {
    void BloomScene();
    void ApplyTonemapping();
    void ApplyFXAA();
    void LensDistortion();
    // GLSL-based, runs on all backends
};
```

---

## 4. Scripting System Architecture

### ZScript Compilation Pipeline

**Source → Bytecode → Execution:**

```
.zs source files
    ↓ (Parse)
ZCC Compiler (src/scripting/zscript/zcc_*.cpp)
    ↓ (Semantic analysis)
Code Generator (src/scripting/backend/codegen.cpp)
    ├─ Type checking & validation
    ├─ Function resolution
    └─ Constant folding
    ↓ (Emit)
VM Bytecode (PCode)
    ↓ (Optional)
JIT Compiler (vm_jit cvar)
    ↓
Execution (FVM::Execute or native code)
```

**Three Scripting Layers:**

1. **DECORATE** (Legacy actor definitions)
   - Declarative syntax for sprites, sounds, states
   - Converted to ZScript internally
   - Example: `ACTOR Player : PlayerChunk { ... }`

2. **ACS** (Action Code Script)
   - Mapper-accessible scripting
   - Script execution triggered by line specials
   - Limited scope (no object creation)

3. **ZScript** (Modern, full-featured)
   - Object-oriented with inheritance
   - Full introspection via reflection system
   - Interop with C++ via vmthunks
   - JIT optional (vm_jit=true)

### VM Thunk System (C++ ↔ ZScript Bridge)

**Virtual Method Binding** (`src/scripting/vmthunks*.cpp`):
```cpp
// Example: actor.Pos property
DEFINE_FIELD_X(Actor, AActor, pos)  // Maps C++ field to ZScript property
DEFINE_ACTION_FUNCTION(AActor, GetPos) { ... }
DEFINE_ACTION_FUNCTION(AActor, SetPos) { ... }
```

**Method Resolution:**
- Functions decorated with `DEFINE_ACTION_FUNCTION` are exposed to ZScript
- Automatic parameter marshalling
- Return value conversion (C++ → VM stack)
- Called via VMValue mechanism (type-erasure)

### Actor System

**Class Hierarchy:**
```
DObject (garbage collected)
    ↓
DThinker (tickable)
    ↓
AActor (game entity)
    ├── Monster actors (Enemy, Boss, etc.)
    ├── Weapon actors
    ├── Pickup items
    └── Projectiles
```

**State System:**
```cpp
// Defines sprite sequences & behavior
enum EStateUseFlags {
    SUF_ACTOR = 1,          // Actor state frame
    SUF_WEAPON = 2,         // Weapon state frame
    SUF_INVENTORY = 4       // Inventory state frame
};

struct FState {
    int sprite;             // Sprite identifier
    BYTE frame;             // Frame within sprite
    SBYTE tics;             // Duration (in ticks)
    void (*action)(...)     // Callback function
};
```

---

## 5. Game Loop Structure

### Main Loop (`D_DoomLoop` in d_main.cpp)

**Frame Timing:**
- Game ticks: 35 Hz (fixed game logic timestep)
- Render frames: 60+ Hz (variable, capped by `vid_maxfps`)
- Decoupled: Multiple renders per game tick allowed

**Per-Frame Sequence:**

```cpp
while (gameloop_abort == false) {
    // INPUT
    D_ProcessEvents();
        ↓ G_Responder(event)
            ↓ Menu/input handling
    
    // LOGIC (if time for next game tick)
    if (gametic update) {
        G_Ticker();
            ├─ Level tick
            ├─ DThinker::TickThinkers() - all actors
            ├─ Player input processing
            └─ Map specials (doors, platforms)
    }
    
    // RENDER
    D_Display();
        ├─ Camera setup (interpolation for smooth motion)
        ├─ R_RenderView() - 3D scene
        │   ├─ Frustum culling
        │   ├─ Visible surface collection
        │   └─ Light culling for shadows
        ├─ SW_RenderScene() or HW_RenderScene()
        │   ├─ Backend-specific rendering
        │   ├─ State machine setup
        │   └─ Draw command generation
        ├─ HUD rendering (D_DrawHUD)
        └─ Menu overlay
    
    // DISPLAY
    screen->Update();      // Present framebuffer
    
    // TIMING
    FPSLimit();            // Enforce vid_maxfps
}
```

### Thinker Execution

**Statnum-based Priority System** (dthinker.cpp):
```cpp
// Thinkers sorted by priority
enum EStatNum {
    STAT_DEFAULT = 0,      // Regular actors
    STAT_PLAYER = 1,
    STAT_MISSILE = 2,      // Projectiles (high priority)
    STAT_BOSSTARGET = 3,
    STAT_LIGHTTRANSFER = 4,
    // ... up to STAT_LIGHT (127)
};

FThinkerCollection {
    FThinkerList Thinkers[128];     // Ordered lists
    FThinkerList FreshThinkers[128]; // New thinkers this frame
    
    void RunThinkers(FLevelLocals *level) {
        for (statnum in 0..127) {
            for (thinker in Thinkers[statnum]) {
                thinker->Tick();  // Virtual function
            }
        }
    }
};
```

**AActor Tick Implementation:**
```cpp
class AActor : public DThinker {
    void Tick() override {
        if (state != nullptr) {
            // State machine: execute action, advance sprite frame
            if (state->action != nullptr) {
                state->action(this);
            }
            if (++tics >= state->tics) {
                SetState(state->next);  // Transition to next state
            }
        }
        // Physics
        UpdatePosition();
        CheckCollisions();
    }
};
```

### Rendering Flow (HW Backend)

**Scene Rendering** (`hwrenderer/scene/hw_drawinfo.cpp`):
```cpp
void PrepareScene(AActor* camera) {
    // 1. Collect visible geometry
    VisibleSpriteList sprites;
    VisibleWallList walls;
    
    // 2. Depth sort
    sort(sprites.begin(), sprites.end(), [](a, b) {
        return a.distance < b.distance;
    });
    
    // 3. State setup per draw batch
    for (wall : walls) {
        SetupWallMaterial(wall);
        RenderState()->SetState(wall.properties);
        DrawWall(wall);
    }
    
    // 4. Transparent geometry (sorted back-to-front)
    for (sprite : sorted_sprites) {
        SetupSpriteMaterial(sprite);
        RenderState()->SetBlendMode(sprite.blend);
        DrawSprite(sprite);
    }
}
```

**Backend Dispatch:**
```cpp
// In FRenderState or backend-specific class
void Draw(int primitiveType, int index, int count) {
    // Backend-specific:
    // - Vulkan:   vkCmdDraw[Indexed]()
    // - OpenGL:   glDrawArrays() / glDrawElements()
    // - Metal:    mtl::render_command_encoder->drawIndexedPrimitives()
}
```

---

## 6. Build Process Details

### CMake Workflow

**Three-Phase Build:**

1. **Configuration Phase:**
   - Feature detection (OpenGL, Vulkan, GLES2)
   - Dependency resolution (SDL2, OpenAL, ZMusic)
   - Compiler flag setup
   - Precompiled header generation

2. **Generation Phase:**
   - Shader source → Include files (for embedded resources)
   - PK3 archives (WAD format) built from source directories
   - Autogenerated code (autosegs, VM thunks)

3. **Compilation:**
   - Parallel compilation (`/MP` on MSVC, `-j` on Unix)
   - Link against static libraries (ZVulkan, ZMusic)
   - Final executable bundled with PK3 resources

### Shader Pipeline

**Compilation Flow:**

```
GLSL/SPIRV source (.glsl)
    ↓ (if Vulkan)
glslang compiler → SPIR-V bytecode
    ↓
Store in VkShaderModule
    ↓
Create pipeline layout + graphics pipeline

(if OpenGL)
    ↓
glGetShaderSource() → compile to ARB bytecode
    ↓
Store in GLProgram
```

**Runtime Shader Compilation:**
- `CompileNextShader()` called per frame if pending
- Asynchronous compilation to avoid frame stalls
- Shader cache per material + render state

---

## Key Files Reference

### Core Engine
- `/src/d_main.cpp` - Main loop, initialization, shutdown
- `/src/g_game.cpp` - Game state, demo playback, save/load
- `/src/common/engine/d_event.cpp` - Event dispatching

### Rendering
- `/src/common/rendering/v_video.cpp` - Framebuffer management
- `/src/common/rendering/hwrenderer/` - Shared HW rendering
- `/src/common/rendering/{gl,vulkan,metal}/` - Backend implementations

### Game Logic
- `/src/playsim/actor.h` - AActor class definition
- `/src/playsim/dthinker.cpp` - Thinker system
- `/src/playsim/p_*.cpp` - Physics, map, sectors

### Scripting
- `/src/scripting/backend/codegen.cpp` - ZScript compiler
- `/src/scripting/vmthunks.cpp` - C++ ↔ ZScript bridge
- `/src/common/scripting/vm.h` - VM execution engine

### Platform
- `/src/common/platform/posix/cocoa/` - macOS Cocoa backend
- `/src/common/platform/posix/sdl/` - SDL2 backend
- `/src/common/platform/win32/` - Windows Win32 backend

---

## Architecture Highlights

**Strengths:**
- Clean abstraction between game logic and rendering
- Multiple rendering backends with identical feature parity
- Powerful scripting system (ZScript) with full introspection
- Modern GPU APIs (Vulkan, Metal) alongside legacy OpenGL
- Modular library architecture (ZMusic, ZVulkan, metal-cpp)

**Decoupling:**
- Game loop independent of render backend selection
- Rendering backend swappable at runtime (with restart)
- Audio system pluggable (multiple MIDI backends)
- Platform abstraction layer allows multiple OS implementations

**Performance Optimizations:**
- Fixed 35 Hz game logic, variable render frame rate
- Lazy shader compilation (asynchronous)
- State machine caching (pipelines, samplers)
- Frustum culling, light culling for shadows
- Deferred GPU resource deletion

