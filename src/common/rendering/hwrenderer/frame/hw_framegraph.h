/*
**  Frame graph -- pass description, dependency graph, topological order
**
**  Phase 2 of the staged plan in docs/frame-graph-resources.md: pure CPU-side
**  bookkeeping over each pass's declared reads/writes, built on the resource
**  registry's stable names (hw_resources.h). No scheduling decision, no
**  allocation, no barrier emission, no backend calls -- that stays gated on
**  Apple Silicon per docs/handoff-framegraph-2026-08-18.md, because that is
**  where the TBDR-vs-IMR risk actually lives. This is the part of the
**  design that doesn't need a GPU to get right.
**
**  Versioning model: a pass "writes" a resource name at the moment it is
**  added -- AddPass order is version-assignment order, exactly as in
**  Unreal's RDG or Frostbite's FrameGraph. A read edge binds to whichever
**  pass most recently wrote that name *at the reading pass's declaration
**  time*. Consequences:
**
**    - Edges always point from an earlier-added pass to a later one, so the
**      graph is acyclic by construction in this phase. Cycle detection is
**      still implemented (Build() reports rather than assumes), because
**      that stops being true once WAR/WAW anti-dependencies are added for
**      the transient allocator.
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
**  Deliberately not yet: reordering passes for scheduling, culling passes
**  whose writes nothing reads, WAR/WAW edges. Those need this piece agreed
**  on first.
*/

#pragma once

#include "tarray.h"

class FString;

struct PassDesc
{
	const char *name = nullptr;	// stable, e.g. "tonemap", "ssao"
	const char *owner = nullptr;	// e.g. "Postprocess", "MtAOModule"
	TArray<const char *> reads;
	TArray<const char *> writes;
};

class FrameGraph
{
public:
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

	// Builds RAW edges from the declared reads/writes and computes a
	// deterministic topological order (Kahn's algorithm, ties broken by
	// declaration index). Non-fatal: problems go in *report*, nothing
	// throws or asserts.
	bool Build(FString *report);

	// Valid after a successful Build(). Pass indices, not PassDesc copies.
	const TArray<int> &Order() const { return mOrder; }
	const PassDesc &Pass(int index) const { return mPasses[index]; }
	int PassCount() const { return (int)mPasses.Size(); }

	void Dump(FString *out) const;

private:
	struct Edge
	{
		int from = -1, to = -1;	// mPasses indices
		const char *resource = nullptr;
	};

	TArray<PassDesc> mPasses;
	TArray<const char *> mExternals;
	TArray<Edge> mEdges;
	TArray<int> mOrder;

	void BuildEdges(FString *report);
	bool TopoSort(FString *report);
};
