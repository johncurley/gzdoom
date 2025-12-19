#pragma once

struct MetalViewSize
{
    float width;
    float height;
};

// Forward declare a C++-friendly function that will be implemented in a .mm file.
MetalViewSize GetMetalViewDrawableSize(void* nsWindowPtr);
