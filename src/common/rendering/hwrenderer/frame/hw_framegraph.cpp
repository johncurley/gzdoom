/*
**  Frame graph -- pass description, dependency graph, topological order
**  See hw_framegraph.h for the versioning model this implements.
*/

#include <cstring>
#include "hwrenderer/frame/hw_framegraph.h"
#include "zstring.h"
#include "printf.h"
#include "c_dispatch.h"
#include "v_video.h"

static bool NameEq(const char *a, const char *b)
{
	return a == b || strcmp(a, b) == 0;
}

void FrameGraph::Reset()
{
	mPasses.Clear();
	mExternals.Clear();
	mEdges.Clear();
	mOrder.Clear();
}

int FrameGraph::AddPass(const PassDesc &desc)
{
	return (int)mPasses.Push(desc);
}

void FrameGraph::DeclareExternal(const char *name)
{
	mExternals.Push(name);
}

void FrameGraph::BuildEdges(FString *report)
{
	mEdges.Clear();

	// name -> index of the pass that most recently wrote it, as of however
	// far BuildEdges has walked mPasses so far. Linear list: frame pass
	// counts are ~10-15, not worth a TMap for this.
	struct Writer { const char *name; int pass; };
	TArray<Writer> lastWriter;

	for (int i = 0; i < (int)mPasses.Size(); i++)
	{
		const PassDesc &pass = mPasses[i];

		// Resolve reads against writers seen *before* this pass -- so a
		// pass that reads and writes the same name (in-place) binds to the
		// previous writer, not itself.
		for (const char *name : pass.reads)
		{
			int writerIndex = -1;
			for (auto &w : lastWriter)
			{
				if (NameEq(w.name, name))
				{
					writerIndex = w.pass;
					break;
				}
			}
			if (writerIndex < 0)
			{
				bool external = false;
				for (const char *ext : mExternals)
				{
					if (NameEq(ext, name))
					{
						external = true;
						break;
					}
				}
				if (!external)
				{
					report->AppendFormat("pass '%s' (%s) reads '%s' before any pass writes it and it isn't declared external\n",
						pass.name, pass.owner, name);
				}
				continue;
			}
			mEdges.Push({ writerIndex, i, name });
		}

		for (const char *name : pass.writes)
		{
			bool updated = false;
			for (auto &w : lastWriter)
			{
				if (NameEq(w.name, name))
				{
					w.pass = i;
					updated = true;
					break;
				}
			}
			if (!updated)
				lastWriter.Push({ name, i });
		}
	}
}

bool FrameGraph::TopoSort(FString *report)
{
	int n = (int)mPasses.Size();
	TArray<int> indegree;
	indegree.Resize(n);
	for (int i = 0; i < n; i++)
		indegree[i] = 0;
	for (auto &e : mEdges)
		indegree[e.to]++;

	TArray<bool> done;
	done.Resize(n);
	for (int i = 0; i < n; i++)
		done[i] = false;

	mOrder.Clear();
	for (int step = 0; step < n; step++)
	{
		int pick = -1;
		for (int i = 0; i < n; i++)
		{
			if (!done[i] && indegree[i] == 0)
			{
				pick = i;
				break;
			}
		}
		if (pick < 0)
		{
			report->AppendFormat("cycle detected: %d pass(es) never reached indegree 0\n", n - step);
			return false;
		}
		mOrder.Push(pick);
		done[pick] = true;
		for (auto &e : mEdges)
		{
			if (e.from == pick)
				indegree[e.to]--;
		}
	}
	return true;
}

bool FrameGraph::Build(FString *report)
{
	*report = "";
	BuildEdges(report);
	bool ok = TopoSort(report);
	return ok && report->Len() == 0;
}

void FrameGraph::Dump(FString *out) const
{
	*out = "";
	out->AppendFormat("%u passes, %u edges\n\n", mPasses.Size(), mEdges.Size());

	out->AppendFormat("  order  pass                 owner            reads -> writes\n");
	for (int idx : mOrder)
	{
		const PassDesc &pass = mPasses[idx];
		FString reads, writes;
		for (unsigned int i = 0; i < pass.reads.Size(); i++)
			reads.AppendFormat("%s%s", i ? ", " : "", pass.reads[i]);
		for (unsigned int i = 0; i < pass.writes.Size(); i++)
			writes.AppendFormat("%s%s", i ? ", " : "", pass.writes[i]);

		out->AppendFormat("  %-7d%-21s%-17s%s -> %s\n",
			idx, pass.name, pass.owner, reads.GetChars(), writes.GetChars());
	}

	if (mEdges.Size() > 0)
	{
		out->AppendFormat("\n  edges:\n");
		for (auto &e : mEdges)
		{
			out->AppendFormat("    %s --[%s]--> %s\n",
				mPasses[e.from].name, e.resource, mPasses[e.to].name);
		}
	}
}

// Self-test: reproduces the Pass2 chain from docs/frame-analysis.md 2
// (tonemap -> colormap -> lens -> fxaa) against real ping-pong buffer names,
// so the versioning model is checked against a known-correct chain before
// anything real gets wired to it.
CCMD(r_framegraph_selftest)
{
	FrameGraph graph;
	// PipelineImage[0] arrives holding the scene render's output, and
	// PaletteTexture is a persistent asset -- both boundary inputs to this
	// sub-graph, not produced by any of these four passes.
	graph.DeclareExternal("PipelineImage[0]");
	graph.DeclareExternal("PaletteTexture");
	graph.AddPass({ "tonemap", "Postprocess", { "PipelineImage[0]", "PaletteTexture" }, { "PipelineImage[1]" } });
	graph.AddPass({ "colormap", "Postprocess", { "PipelineImage[1]" }, { "PipelineImage[0]" } });
	graph.AddPass({ "lens", "Postprocess", { "PipelineImage[0]" }, { "PipelineImage[1]" } });
	graph.AddPass({ "fxaa", "Postprocess", { "PipelineImage[1]" }, { "PipelineImage[0]" } });

	FString report;
	bool ok = graph.Build(&report);

	FString dump;
	graph.Dump(&dump);
	Printf("%s\n", dump.GetChars());

	if (!ok)
		Printf("Build() reported:\n%s\n", report.GetChars());

	bool orderMatchesDeclaration = true;
	for (int i = 0; i < graph.PassCount(); i++)
		orderMatchesDeclaration &= (graph.Order()[i] == i);
	Printf(ok && orderMatchesDeclaration ? "selftest: PASS\n" : "selftest: FAIL\n");
}

// Real per-frame data: whatever GLPPRenderState::Draw()/VkPPRenderState::Draw()
// recorded via AddPass() since the last Graph().Reset() (once per frame, next to
// Resources().BeginFrame()). Covers tonemap/colormap/lens/fxaa (always nameable,
// via the special PPTextureType names) plus ssao/exposure/bloom/blur (nameable
// since PPTexture::Name -- see NameAndDeclare in hw_postprocess.cpp) -- not yet
// shadowmap or custom shaders, still raw PPTexture* with no name. Mirrors
// CCMD(r_resources)'s shape (hw_resources.cpp): dump unconditionally, build's
// report is a real defect signal here (unlike ValidateFrame's expected-noise
// "untouched" case), so it's always shown when non-empty, not gated behind a cvar.
CCMD(r_framegraph)
{
	if (!screen)
	{
		Printf("No render backend active.\n");
		return;
	}

	FrameGraph &graph = screen->Graph();

	// Every one of these predates this partial graph -- produced by the scene
	// render pass (PipelineImage[0], Scene*) or a one-time CPU upload
	// (PaletteTexture, AO.RandomTexture*) rather than by any AddPass() here.
	// Real external boundaries, not missing passes. PipelineImage[1] is not
	// listed: tonemap always writes it before anything reads it, in every
	// real ordering, so declaring it external would only mask a genuine
	// ordering bug if one ever appeared.
	graph.DeclareExternal("PipelineImage[0]");
	graph.DeclareExternal("SceneColor");
	graph.DeclareExternal("SceneNormal");
	graph.DeclareExternal("SceneDepthStencil");
	graph.DeclareExternal("SceneFog");
	graph.DeclareExternal("PaletteTexture");
	graph.DeclareExternal("AO.RandomTexture0");
	graph.DeclareExternal("AO.RandomTexture1");
	graph.DeclareExternal("AO.RandomTexture2");

	FString report;
	bool ok = graph.Build(&report);

	FString dump;
	graph.Dump(&dump);
	Printf("%s\n", dump.GetChars());

	if (!ok && report.Len() > 0)
		Printf("Build() reported:\n%s\n", report.GetChars());
}
