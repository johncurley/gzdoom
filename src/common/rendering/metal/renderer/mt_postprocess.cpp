/*
**  Metal backend - Post-processing
**  Copyright (c) 2025 GZDoom Contributors
*/

#include "mt_postprocess.h"
#include "metal/system/mt_renderdevice.h"

MtPostprocess::MtPostprocess(MetalRenderDevice* fb) : fb(fb) {}
MtPostprocess::~MtPostprocess() {}

void MtPostprocess::BlurScene(float amount) {}
void MtPostprocess::AmbientOccludeScene(float m5) {}
void MtPostprocess::UpdateShadowMap() {}
void MtPostprocess::SetSceneRenderTarget(bool useSSAO) {}
void MtPostprocess::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D) {}
