/*
**  Frame graph resource registry -- phase 1 (record + report, no allocation)
**
**  Design: docs/frame-graph-resources.md. Backend-neutral description of the
**  textures a frame's renderer already creates, keyed by a stable literal
**  name. The registry never allocates or owns GPU memory in this phase -- it
**  is bookkeeping over what the backend made, so a first answer exists for
**  "how much memory does a frame use" and "which resources did this frame
**  actually touch" without needing the graph/scheduler this is step one of.
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include "tarray.h"

class FString;

enum class ResourceFormat : uint8_t
{
	Unknown, RGBA8, RGBA16F, R8, RG16F, R32F, D24S8
};

// How the size is DERIVED, not just what it is -- this is what makes a
// mismatch checkable: two resources both claiming SceneScaled(4) must agree,
// and a consumer expecting SceneFull must not silently be handed SceneScaled(2).
struct SizeRule
{
	enum Kind : uint8_t { Fixed, SceneFull, SceneScaled, MipOf } kind = Fixed;
	int divisor = 1;			// SceneScaled
	int level = 0;				// MipOf
	const char *parent = nullptr;	// MipOf
};

struct ResourceDesc
{
	const char *name = nullptr;	// stable, e.g. "SceneColor", "AO.Ambient0"
	const char *owner = nullptr;	// e.g. "VkRenderBuffers", "MtAOModule"
	int width = 0, height = 0;
	int samples = 1;
	ResourceFormat format = ResourceFormat::Unknown;
	SizeRule size;
	bool transient = false;	// dead at end of frame -- future aliasing candidate
};

// Backend-neutral, backend-owned memory: `backendHandle` is opaque
// (MTL::Texture*, VkImage, a GL name). The registry never dereferences it --
// it exists so a dump can be correlated with a GPU capture.
class FrameResources
{
public:
	// Called where the texture is created today. Re-declaring the same name
	// with different dimensions is the recreate path and is expected.
	void Declare(const ResourceDesc &desc, void *backendHandle);
	void Forget(const char *name);

	// Called where the resource is bound/written. Cheap: a frame counter store.
	void Touch(const char *name, bool asOutput);

	void BeginFrame(int sceneWidth, int sceneHeight);

	// Non-fatal by default: reports, does not throw. See docs/frame-graph-resources.md
	// "failure policy" -- a false positive that kills the frame is worse than
	// the bug it would catch.
	void ValidateFrame(FString *report) const;

	void Dump(FString *out) const;
	size_t TotalBytes() const;

private:
	struct Entry
	{
		ResourceDesc desc;
		void *backendHandle = nullptr;
		uint64_t declaredFrame = 0;
		uint64_t lastWriteFrame = 0;
		uint64_t lastReadFrame = 0;
	};

	TArray<Entry> mEntries;
	uint64_t mFrameCounter = 0;
	int mSceneWidth = 0;
	int mSceneHeight = 0;

	Entry *Find(const char *name);
	const Entry *Find(const char *name) const;
	static size_t BytesPerPixel(ResourceFormat format);
	static const char *FormatName(ResourceFormat format);
};
