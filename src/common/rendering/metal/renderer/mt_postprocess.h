#pragma once

#include <memory>
#include <functional>

class MetalRenderDevice;
class MtTextureImage;

// Post-processing effects
class MtPostprocess
{
public:
	MtPostprocess(MetalRenderDevice* fb);
	~MtPostprocess();

	// Post-processing operations
	void BlurScene(float amount);
	void AmbientOccludeScene(float m5);
	void UpdateShadowMap();

	// Scene rendering
	void SetSceneRenderTarget(bool useSSAO);
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()>& afterBloomDrawEndScene2D);

private:
	MetalRenderDevice* fb = nullptr;
};
