/*
**  Frame graph -- pass description, dependency graph, topological order
**
**  CPU-side dependency analysis over each pass's declared reads/writes, built
**  on the resource registry's stable names (hw_resources.h). It reports
**  dependencies, required-output reachability, and within-frame lifetimes for
**  explicitly transient resources. Pass2 consumes this order for recorded
**  draws; the graph does not allocate, alias, emit backend barriers, or call a
**  backend. Metal/TBDR execution policy remains gated on Apple Silicon evidence.
**
**  Versioning model: a pass "writes" a resource name at the moment it is
**  added -- AddPass order is version-assignment order, exactly as in
**  Unreal's RDG or Frostbite's FrameGraph. A read edge binds to whichever
**  pass most recently wrote that name *at the reading pass's declaration
**  time*. Consequences:
**
**    - Attachment loads, depth tests, and blending that preserve prior
**      contents are logical reads: list the attachment in both reads and
**      writes. The backend-use record may still be a write-only attachment
**      bind; it describes the binding, not every implicit pixel dependency.
**    - Edges always point from an earlier-added pass to a later one, so the
**      graph is acyclic by construction in this versioning model. Cycle
**      detection remains in Build() as a defensive check if versioning rules
**      change later.
**    - Same-name rewrites -- PipelineImage[0]/[1] ping-ponging across four
**      or five passes in one frame -- are handled correctly: each rewrite
**      installs a new "most recent writer", so a read binds to whichever
**      version was live at that point, not to the first or last writer of
**      that name in the whole frame.
**    - A read of a name nobody has written yet is a real bug (frame-analysis
**      .md 3.1's class of defect: a pass depending on state nothing
**      declared). Build() reports it rather than crashing -- same failure
**      policy as FrameResources::ValidateFrame.
**
**  The graph records logical RAW, WAR, and WAW dependencies. These edges
**  constrain CPU pass order; they do not replace backend resource transitions.
**  Lifetimes remain diagnostics, not permission to alias.
*/

#pragma once

#include <cstdint>
#include "tarray.h"
#include "zstring.h"

class FString;

enum class FrameGraphAccess : uint8_t
{
	Read,
	Write,
	ReadWrite
};

// Backend-neutral operation categories. These are deliberately not Vulkan
// enums: the same graph contract will be usable by GL and Metal later.
enum class FrameGraphUsage : uint8_t
{
	Sampled,
	ColorAttachment,
	DepthStencilAttachment,
	TransferSource,
	TransferDestination,
	Storage,
	Present
};

struct ResourceUse
{
	const char *name = nullptr;
	FrameGraphAccess access = FrameGraphAccess::Read;
	FrameGraphUsage usage = FrameGraphUsage::Sampled;
};

struct PassDesc
{
	const char *name = nullptr;	// stable, e.g. "tonemap", "ssao"
	const char *owner = nullptr;	// e.g. "Postprocess", "MtAOModule"
	TArray<const char *> reads;
	TArray<const char *> writes;
	// Backend-observed bindings. The logical read/write lists may include an
	// implicit prior-contents read (attachment load/blend) even when the bind
	// hook observes only the output attachment write.
	TArray<ResourceUse> uses;
	bool keepAlive = false;		// external side effect or cross-frame result
};

struct FrameGraphLifetime
{
	const char *resource = nullptr;
	int firstOrder = -1;	// position in the deterministic topological order
	int lastOrder = -1;
};

enum class FrameGraphPreparation : uint8_t
{
	RenderThread,
	Worker
};

// Observation of an upload that the backend has already recorded. These
// fields describe facts about existing work; they do not submit or synchronize
// GPU work. stagingRetained means the source remains valid until the backend
// consumes it (including APIs that synchronously consume client memory).
struct FrameGraphUploadDesc
{
	const char *resource = nullptr;
	const char *owner = nullptr;
	FrameGraphPreparation preparation = FrameGraphPreparation::RenderThread;
	bool stagingRetained = false;
	bool transferRecorded = false;
	bool orderedBeforeConsumers = false;
};

class FrameGraph
{
public:
	// Clears pass/per-frame declarations and analysis. Unconsumed uploads
	// persist across the reset; transient classifications are reimported from
	// FrameResources when the diagnostic graph is built.
	void Reset();

	// Passes must be added in a legal sequence for now -- see the versioning
	// note above. Returns the pass's index, stable until the next Reset().
	int AddPass(const PassDesc &desc);

	// Marks a name as a legitimate boundary input -- produced by something
	// outside this graph (the scene render, a persistent asset like
	// PaletteTexture) rather than by a missing pass. Call before Build().
	// Without this, every graph that doesn't start from nothing would fail
	// Build()'s missing-writer check on its very first read.
	void DeclareExternal(const char *name);

	// Names a resource whose final value must be produced by this frame graph.
	// The latest writer becomes a liveness root during diagnostic analysis.
	void DeclareOutput(const char *name);

	// Marks a backend-owned resource as transient for within-frame lifetime
	// reporting. Imported and persistent resources must not be declared transient.
	void DeclareTransient(const char *name);

	// Makes a derived name stable until Reset(). Pass descriptions retain name
	// pointers, so callback-local strings are not safe when multiple targets are
	// recorded in one frame.
	const char *MakeOwnedResourceName(const char *base, const char *suffix);

	// Declares two names as the same physical resource for dependency
	// validation. This is needed for backend layouts that expose one object
	// under multiple stable names, such as GL's non-MSAA SceneColor /
	// PipelineImage[0] alias.
	void DeclareAlias(const char *name, const char *canonical);

	// Record an already-submitted upload and sampled-resource observations.
	// Uploads can predate the current frame; they remain pending until a sampled
	// read is observed. Build() checks the upload facts, and Dump() shows whether
	// a consumer read followed the upload.
	void RecordUpload(const FrameGraphUploadDesc &desc);
	void ObserveResourceRead(const char *name);
	// Records the upload observation and attaches a deduplicated sampled read
	// to an active scene, offscreen, or UI pass. A resource with an earlier
	// graph writer remains graph-produced; otherwise its current value is an
	// imported input to this frame.
	void ObserveSceneMaterialRead(const char *name);

	// Builds RAW/WAR/WAW edges from the declared reads/writes and computes a
	// deterministic topological order (Kahn's algorithm, ties broken by
	// declaration index). Non-fatal: problems go in *report*, nothing
	// throws or asserts.
	bool Build(FString *report);

	// Valid after a successful Build(). Pass indices, not PassDesc copies.
	const TArray<int> &Order() const { return mOrder; }
	const PassDesc &Pass(int index) const { return mPasses[index]; }
	int PassCount() const { return (int)mPasses.Size(); }
	int EdgeCount() const { return (int)mEdges.Size(); }
	const TArray<int> &DeadPassCandidates() const { return mDeadPassCandidates; }
	const TArray<FrameGraphLifetime> &Lifetimes() const { return mLifetimes; }

	// Backend observation hooks. These record what the existing renderer did;
	// they do not emit barriers or alter execution.
	void BeginBackendPass(int passIndex);
	void ObserveBackendUse(const char *name, FrameGraphAccess access, FrameGraphUsage usage);
	void EndBackendPass();

	void Dump(FString *out) const;

private:
	enum class EdgeType : uint8_t { RAW, WAR, WAW };
	struct Edge
	{
		int from = -1, to = -1;	// mPasses indices
		const char *resource = nullptr;
		EdgeType type = EdgeType::RAW;
	};

	struct ObservedUse
	{
		int pass = -1;
		ResourceUse use;
	};
	struct UploadObservation
	{
		FString resource;
		FString owner;
		FrameGraphPreparation preparation = FrameGraphPreparation::RenderThread;
		bool stagingRetained = false;
		bool transferRecorded = false;
		bool orderedBeforeConsumers = false;
		uint64_t sequence = 0;
	};
	TArray<PassDesc> mPasses;
	TArray<const char *> mExternals;
	TArray<const char *> mOutputs;
	TArray<const char *> mTransientResources;
	struct Alias
	{
		const char *name;
		const char *canonical;
	};
	TArray<Alias> mAliases;
	TArray<Edge> mEdges;
	TArray<int> mOrder;
	TArray<int> mDeadPassCandidates;
	TArray<FrameGraphLifetime> mLifetimes;
	TArray<uint8_t> mBackendObserved;
	TArray<ObservedUse> mObservedUses;
	TArray<UploadObservation> mUploads;
	TArray<FString> mOwnedSceneReadNames;
	TArray<FString> mOwnedResourceNames;
	TMap<FString, uint64_t> mResourceReads;
	uint64_t mObservationSequence = 0;
	int mActivePass = -1;

	const char *CanonicalName(const char *name) const;
	void AnalyzeLiveness(FString *report);
	void AnalyzeLifetimes();
	void BuildEdges(FString *report);
	bool TopoSort(FString *report);
	void ValidateUses(FString *report) const;
	void ValidateUploads(FString *report) const;
};
