#pragma once

struct MetalViewSize
{
    float width;         // Physical pixels (backing scale applied)
    float height;
    float logicalWidth;  // Logical points (what the engine uses for coords)
    float logicalHeight;
};

// Metal buffer binding points
enum MtBufferIndex {
    MT_Buffer_Vertex0 = 0,
    MT_Buffer_Vertex1 = 1,
    MT_Buffer_GlobalBase = 10, // GZDoom global buffers (Light, Viewpoint, etc.) start here
    MT_Buffer_Matrices = 20,
    MT_Buffer_Stream = 21,
    MT_Buffer_PushConstants = 7, // Matches current shader binding 7
};

// Helper to translate GZDoom binding points to Metal buffer indices
inline int MtTranslateBindingPoint(int bp) {
    if (bp < 0) return bp;
    return bp + MT_Buffer_GlobalBase;
}

// Forward declare a C++-friendly function that will be implemented in a .mm file.
MetalViewSize GetMetalViewDrawableSize(void* nsWindowPtr);
