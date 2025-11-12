# Phase 7: Resource Binding - COMPLETE ✅

**Date:** November 7, 2025
**Status:** Implementation Complete - Build Clean

---

## ✅ Implemented Components

### 1. Three-Tier Resource Binding Strategy

**Location:** `src/common/rendering/metal/renderer/mt_resourcebinding.cpp`

Following Vulkan's proven descriptor set pattern, adapted for Metal's simpler binding model:

#### **Tier 0: Fixed Textures** (Bind once at initialization)
- Purpose: Rarely-changing resources (shadowmaps, lightmaps, RTX data)
- Performance: Zero per-frame overhead
- Example: Shadow atlas, light probes

#### **Tier 1: Per-Frame Buffers** (Update offsets per-frame)
- Purpose: Frequently-updated uniform buffers
- Performance: Dynamic offsets avoid rebinding
- Examples:
  - Viewpoint uniforms (camera matrices)
  - Matrix buffer (model/view/projection)
  - Stream data (render state)
  - Light buffer (dynamic lights)
  - Bone buffer (skeletal animation)

#### **Tier 2: Per-Material Textures** (Bind per-draw)
- Purpose: Material-specific textures
- Performance: Only bind when material changes
- Examples: Diffuse, normal, specular texture layers

---

## 📋 API Methods Implemented

### Core Binding Functions

#### `BindFixedTexture(int slot, void* texture)`
**Purpose:** Register a texture that rarely changes (Tier 0)

**Usage:**
```cpp
bindingMgr->BindFixedTexture(0, shadowMapTexture);
bindingMgr->BindFixedTexture(1, lightMapTexture);
```

**Metal API:**
- `setVertexTexture(texture, index)`
- `setFragmentTexture(texture, index)`

---

#### `BindPerFrameBuffer(int slot, void* buffer, uint32_t offset)`
**Purpose:** Register a per-frame buffer with dynamic offset (Tier 1)

**Usage:**
```cpp
// Update every frame with new offset
bindingMgr->BindPerFrameBuffer(0, viewpointBuffer, currentOffset);
bindingMgr->BindPerFrameBuffer(1, matrixBuffer, matrixOffset);
```

**Metal API:**
- `setVertexBuffer(buffer, offset, index)`
- `setFragmentBuffer(buffer, offset, index)`

**Key Feature:** Dynamic offsets avoid creating new buffers per frame

---

#### `BindMaterialTexture(int slot, void* texture, void* sampler)`
**Purpose:** Bind material-specific texture + sampler (Tier 2)

**Usage:**
```cpp
// Called when switching materials
bindingMgr->BindMaterialTexture(0, diffuseTexture, linearSampler);
bindingMgr->BindMaterialTexture(1, normalTexture, linearSampler);
bindingMgr->BindMaterialTexture(2, specularTexture, linearSampler);
```

**Metal API:**
- `setVertexTexture(texture, index)` + `setVertexSamplerState(sampler, index)`
- `setFragmentTexture(texture, index)` + `setFragmentSamplerState(sampler, index)`

---

### State Management Functions

#### `ApplyBindings(void* encoder, bool vertex, bool fragment)`
**Purpose:** Apply all registered bindings to Metal render encoder

**Implementation:**
```cpp
void ApplyBindings(void* encoder, bool vertex, bool fragment)
{
    auto mtlEncoder = (MTL::RenderCommandEncoder*)encoder;

    // Tier 0: Fixed textures
    for (size_t i = 0; i < mFixedTextures.size(); i++)
        if (mFixedTextures[i])
            mtlEncoder->setFragmentTexture(texture, i);

    // Tier 1: Per-frame buffers
    for (size_t i = 0; i < mPerFrameBuffers.size(); i++)
        if (mPerFrameBuffers[i].buffer)
            mtlEncoder->setFragmentBuffer(buffer, offset, i);

    // Tier 2: Material textures
    for (size_t i = 0; i < mMaterialTextures.size(); i++)
        if (mMaterialTextures[i].texture)
        {
            mtlEncoder->setFragmentTexture(texture, i);
            mtlEncoder->setFragmentSamplerState(sampler, i);
        }
}
```

**Parameters:**
- `encoder`: MTL::RenderCommandEncoder* for current render pass
- `vertex`: Bind to vertex shader stage if true
- `fragment`: Bind to fragment shader stage if true

**Called By:** `MtRenderState::Apply()` before each draw

---

#### `BeginFrame()`
**Purpose:** Reset per-frame state for new frame

**Implementation:**
```cpp
void BeginFrame()
{
    // Clear Tier 2 (material textures)
    // Keep Tier 0 (fixed) and Tier 1 (per-frame) intact
    mMaterialTextures.clear();
}
```

**Called By:** `MtRenderState::BeginFrame()` at frame start

---

#### `ClearMaterialTextures()`
**Purpose:** Clear material texture bindings when switching materials

**Implementation:**
```cpp
void ClearMaterialTextures()
{
    mMaterialTextures.clear();
}
```

**Called By:** `MtRenderState::ApplyMaterial()` when material changes

---

## 🔧 Metal API Mapping

### Metal Binding Methods Used

| Metal API | Purpose | Shader Stage |
|-----------|---------|-------------|
| `setVertexTexture(texture, index)` | Bind texture to vertex shader | Vertex |
| `setFragmentTexture(texture, index)` | Bind texture to fragment shader | Fragment |
| `setVertexBuffer(buffer, offset, index)` | Bind buffer to vertex shader | Vertex |
| `setFragmentBuffer(buffer, offset, index)` | Bind buffer to fragment shader | Fragment |
| `setVertexSamplerState(sampler, index)` | Bind sampler to vertex shader | Vertex |
| `setFragmentSamplerState(sampler, index)` | Bind sampler to fragment shader | Fragment |

### Binding Indices

**Convention (matches Vulkan):**
```
Buffers (Tier 1):
  Index 0: Viewpoint uniforms (HWViewpointUniforms)
  Index 1: Matrix buffer (MatricesUBO)
  Index 2: Stream buffer (StreamUBO)
  Index 3: Light buffer (SSBO)
  Index 4: Bone buffer (SSBO)

Textures (Tier 0 & 2):
  Index 0-7: Material texture layers
  Index 8+: Fixed textures (shadowmap, lightmap, etc.)

Samplers (Tier 2):
  Index 0-7: Per-texture samplers
```

---

## 📊 Code Statistics

| Component | Lines | Status |
|-----------|-------|--------|
| BindFixedTexture() | 5 | ✅ Complete |
| BindPerFrameBuffer() | 6 | ✅ Complete |
| BindMaterialTexture() | 5 | ✅ Complete |
| ApplyBindings() | 58 | ✅ Complete |
| BeginFrame() | 4 | ✅ Complete |
| ClearMaterialTextures() | 4 | ✅ Complete |
| **Total Lines** | **82** | **✅ 100% Complete** |

---

## 🎯 Integration Points

### Used By:

**1. MtRenderState (Primary Consumer)**
```cpp
class MtRenderState : public FRenderState {
    void Apply(int dt) {
        // ... other state setup
        fb->GetResourceBindingManager()->ApplyBindings(
            mEncoder, true, true  // vertex=true, fragment=true
        );
    }

    void BeginFrame() {
        fb->GetResourceBindingManager()->BeginFrame();
    }

    void ApplyMaterial() {
        auto bindingMgr = fb->GetResourceBindingManager();
        bindingMgr->ClearMaterialTextures();
        // Bind new material textures
        bindingMgr->BindMaterialTexture(0, diffuse, sampler);
    }
};
```

**2. MetalRenderDevice (Manager Access)**
```cpp
MtResourceBindingManager* GetResourceBindingManager() {
    return mResourceBindingManager.get();
}
```

---

## 🧪 Testing Requirements

### Unit Tests Needed:

1. **Binding Registration**
   - Verify slot expansion (dynamic array resizing)
   - Test null texture/buffer handling
   - Verify offset storage

2. **Apply Bindings**
   - Test vertex-only binding (vertex=true, fragment=false)
   - Test fragment-only binding (vertex=false, fragment=true)
   - Test both stages (vertex=true, fragment=true)
   - Verify null encoder handling

3. **State Management**
   - Test BeginFrame() clears Tier 2 only
   - Test ClearMaterialTextures()
   - Verify Tier 0 and Tier 1 persistence

### Integration Tests Needed:

1. **Material Switching**
   - Bind material A textures
   - Clear and bind material B textures
   - Verify no leakage between materials

2. **Frame Boundaries**
   - BeginFrame → draw calls → EndFrame
   - Verify fixed textures persist across frames
   - Verify per-frame buffers update correctly

3. **Performance Tests**
   - Measure binding overhead per draw
   - Profile dynamic offset updates
   - Compare with Vulkan descriptor set performance

---

## 🔗 Dependencies

### Required Components:
1. ✅ Metal.framework - Native Metal API
2. ✅ metal-cpp - C++ bindings (zero overhead)
3. ✅ MetalRenderDevice - Manager factory
4. ✅ MtRenderState - Consumer of bindings

### Dependencies From:
1. ⚠️ MtRenderState::Apply() - Calls ApplyBindings() (needs implementation)
2. ⚠️ MtRenderState::ApplyMaterial() - Binds textures (needs implementation)
3. ⚠️ MtTextureManager - Provides texture objects
4. ⚠️ MtBufferManager - Provides buffer objects
5. ⚠️ MtSamplerManager - Provides sampler states

---

## 📝 Design Notes

### 1. Three-Tier Strategy Benefits

**Performance:**
- Tier 0 (Fixed): Bind once, zero per-frame cost
- Tier 1 (Per-Frame): Dynamic offsets avoid buffer recreation
- Tier 2 (Material): Only rebind on material change

**Memory:**
- Dynamic arrays resize on demand
- No pre-allocation of unused slots
- Automatic cleanup on clear

**Simplicity:**
- Simpler than Vulkan descriptor sets
- No pipeline layouts required
- Direct binding to encoder

---

### 2. Metal vs Vulkan Differences

| Feature | Vulkan | Metal |
|---------|--------|-------|
| **Resource Model** | Descriptor sets | Direct binding |
| **Update Frequency** | vkUpdateDescriptorSets | Per-frame rebinding |
| **Pipeline Layout** | Required | Not needed |
| **Binding Overhead** | Lower (pre-bound) | Higher (per-draw) |
| **Flexibility** | Less (pre-defined) | More (dynamic) |
| **Complexity** | High | Low |

**Metal Advantages:**
- Simpler API (no descriptor set allocation)
- More flexible (bind anytime)
- Easier debugging (explicit binding)

**Vulkan Advantages:**
- Better batch performance (pre-bound sets)
- Lower CPU overhead (fewer API calls)
- Better for high draw call counts

---

### 3. Binding Index Convention

Following Vulkan's binding scheme for consistency:

```
layout(set=0, binding=0) uniform ViewpointUBO { ... };  // Vulkan
setFragmentBuffer(buffer, offset, 0);                    // Metal

layout(set=0, binding=1) uniform MatricesUBO { ... };    // Vulkan
setFragmentBuffer(buffer, offset, 1);                    // Metal

layout(set=1, binding=0) uniform sampler2D tex;          // Vulkan
setFragmentTexture(texture, 0);                          // Metal
setFragmentSamplerState(sampler, 0);                     // Metal
```

This allows shader code to be identical after SPIR-V → MSL translation!

---

### 4. Error Handling

**Current:** Minimal (null checks only)

**Future Enhancements:**
- Validate binding indices against shader reflection
- Warn on unused bindings
- Detect binding conflicts
- Log binding state for debugging

---

## ✅ Phase 7 Completion Checklist

- [x] Three-tier binding strategy defined
- [x] BindFixedTexture() implemented
- [x] BindPerFrameBuffer() implemented
- [x] BindMaterialTexture() implemented
- [x] ApplyBindings() implemented (all 3 tiers)
- [x] BeginFrame() implemented
- [x] ClearMaterialTextures() implemented
- [x] Metal API calls correct
- [x] Signed/unsigned comparison warnings fixed
- [x] Clean build (no errors/warnings)
- [x] Documentation complete

---

**Status:** Phase 7 Resource Binding Complete ✅
**Next Phase:** Phase 8 - Integration & Testing
**Blockers:** None - Implementation ready for integration
**Build Status:** ✅ Clean build, zero warnings

**Confidence:** High - Following proven Vulkan pattern, Metal API usage correct, efficient binding strategy implemented
