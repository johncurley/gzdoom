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

static const char *AccessName(FrameGraphAccess access)
{
	switch (access)
	{
	case FrameGraphAccess::Read:      return "read";
	case FrameGraphAccess::Write:     return "write";
	case FrameGraphAccess::ReadWrite: return "read/write";
	}
	return "unknown";
}

static const char *UsageName(FrameGraphUsage usage)
{
	switch (usage)
	{
	case FrameGraphUsage::Sampled:                return "sampled";
	case FrameGraphUsage::ColorAttachment:       return "color-attachment";
	case FrameGraphUsage::DepthStencilAttachment: return "depth-stencil";
	case FrameGraphUsage::TransferSource:        return "transfer-source";
	case FrameGraphUsage::TransferDestination:   return "transfer-destination";
	case FrameGraphUsage::Storage:               return "storage";
	case FrameGraphUsage::Present:               return "present";
	}
	return "unknown";
}

void FrameGraph::Reset()
{
	mPasses.Clear();
	mExternals.Clear();
	mAliases.Clear();
	mEdges.Clear();
	mOrder.Clear();
	mBackendObserved.Clear();
	mObservedUses.Clear();
	mActivePass = -1;
}

int FrameGraph::AddPass(const PassDesc &desc)
{
	int index = (int)mPasses.Push(desc);
	mBackendObserved.Push(0);
	return index;
}

void FrameGraph::DeclareExternal(const char *name)
{
	mExternals.Push(name);
}

void FrameGraph::DeclareAlias(const char *name, const char *canonical)
{
	if (!name || !canonical || NameEq(name, canonical))
		return;
	for (const Alias &alias : mAliases)
		if (NameEq(alias.name, name) && NameEq(alias.canonical, canonical))
			return;
	mAliases.Push({ name, canonical });
}

const char *FrameGraph::CanonicalName(const char *name) const
{
	for (int depth = 0; depth < (int)mAliases.Size(); depth++)
	{
		bool found = false;
		for (const Alias &alias : mAliases)
		{
			if (NameEq(alias.name, name))
			{
				name = alias.canonical;
				found = true;
				break;
			}
		}
		if (!found)
			break;
	}
	return name;
}

void FrameGraph::BeginBackendPass(int passIndex)
{
	mActivePass = -1;
	if (passIndex >= 0 && passIndex < (int)mPasses.Size())
	{
		mActivePass = passIndex;
		mBackendObserved[passIndex] = 1;
	}
}

void FrameGraph::ObserveBackendUse(const char *name, FrameGraphAccess access, FrameGraphUsage usage)
{
	if (mActivePass >= 0)
		mObservedUses.Push({ mActivePass, { name, access, usage } });
}

void FrameGraph::EndBackendPass()
{
	mActivePass = -1;
}

void FrameGraph::ValidateUses(FString *report) const
{
	for (int passIndex = 0; passIndex < (int)mPasses.Size(); passIndex++)
	{
		const PassDesc &pass = mPasses[passIndex];
		for (const ResourceUse &use : pass.uses)
		{
			if (!use.name)
			{
				report->AppendFormat("pass '%s' (%s) has a null resource use name\n", pass.name, pass.owner);
				continue;
			}

			const char *canonicalName = CanonicalName(use.name);
			bool isRead = false;
			bool isWrite = false;
			for (const char *name : pass.reads)
				isRead |= NameEq(CanonicalName(name), canonicalName);
			for (const char *name : pass.writes)
				isWrite |= NameEq(CanonicalName(name), canonicalName);
			bool roleMatches =
				(use.access == FrameGraphAccess::Read && isRead && !isWrite) ||
				(use.access == FrameGraphAccess::Write && isWrite && !isRead) ||
				(use.access == FrameGraphAccess::ReadWrite && isRead && isWrite);
			if (!roleMatches)
			{
				report->AppendFormat("pass '%s' (%s) declares %s use '%s' but its read/write lists disagree\n",
					pass.name, pass.owner, AccessName(use.access), use.name);
			}
		}

		if (!mBackendObserved[passIndex])
			continue;

		TArray<bool> matched;
		matched.Resize(pass.uses.Size());
		for (unsigned int i = 0; i < matched.Size(); i++)
			matched[i] = false;

		for (const ObservedUse &observed : mObservedUses)
		{
			if (observed.pass != passIndex)
				continue;

			int match = -1;
			for (unsigned int i = 0; i < pass.uses.Size(); i++)
			{
				if (!matched[i] &&
					NameEq(CanonicalName(pass.uses[i].name), CanonicalName(observed.use.name)) &&
					pass.uses[i].access == observed.use.access &&
					pass.uses[i].usage == observed.use.usage)
				{
					match = (int)i;
					break;
				}
			}
			if (match < 0)
			{
				report->AppendFormat("pass '%s' (%s) observed undeclared %s %s use '%s'\n",
					pass.name, pass.owner, AccessName(observed.use.access), UsageName(observed.use.usage), observed.use.name);
			}
			else
			{
				matched[match] = true;
			}
		}

		for (unsigned int i = 0; i < pass.uses.Size(); i++)
		{
			if (!matched[i])
			{
				report->AppendFormat("pass '%s' (%s) declared but did not observe %s %s use '%s'\n",
					pass.name, pass.owner, AccessName(pass.uses[i].access), UsageName(pass.uses[i].usage), pass.uses[i].name);
			}
		}
	}
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
			const char *canonicalName = CanonicalName(name);
			int writerIndex = -1;
			for (auto &w : lastWriter)
			{
				if (NameEq(w.name, canonicalName))
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
					if (NameEq(CanonicalName(ext), canonicalName))
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
			const char *canonicalName = CanonicalName(name);
			bool updated = false;
			for (auto &w : lastWriter)
			{
				if (NameEq(w.name, canonicalName))
				{
					w.pass = i;
					updated = true;
					break;
				}
			}
			if (!updated)
				lastWriter.Push({ canonicalName, i });
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
	ValidateUses(report);
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
		for (const ResourceUse &use : pass.uses)
			out->AppendFormat("           use: %-12s %-18s %s\n", AccessName(use.access), UsageName(use.usage), use.name);
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

	// Exercise the first backend-use contract independently of the live Vulkan
	// path: declared uses must agree with the read/write roles, and the backend
	// observation hooks must match them exactly.
	FrameGraph useGraph;
	useGraph.DeclareExternal("SceneColor");
	useGraph.DeclareExternal("StorageImage");
	PassDesc resolvePass;
	resolvePass.name = "resolve-test";
	resolvePass.owner = "selftest";
	resolvePass.reads.Push("SceneColor");
	resolvePass.writes.Push("PipelineImage[0]");
	resolvePass.uses.Push({ "SceneColor", FrameGraphAccess::Read, FrameGraphUsage::TransferSource });
	resolvePass.uses.Push({ "PipelineImage[0]", FrameGraphAccess::Write, FrameGraphUsage::TransferDestination });
	int resolvePassIndex = useGraph.AddPass(resolvePass);
	useGraph.BeginBackendPass(resolvePassIndex);
	useGraph.ObserveBackendUse("SceneColor", FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
	useGraph.ObserveBackendUse("PipelineImage[0]", FrameGraphAccess::Write, FrameGraphUsage::TransferDestination);
	useGraph.EndBackendPass();

	PassDesc depthPass;
	depthPass.name = "depth-test";
	depthPass.owner = "selftest";
	depthPass.writes.Push("DepthTarget");
	depthPass.uses.Push({ "DepthTarget", FrameGraphAccess::Write, FrameGraphUsage::DepthStencilAttachment });
	int depthPassIndex = useGraph.AddPass(depthPass);
	useGraph.BeginBackendPass(depthPassIndex);
	useGraph.ObserveBackendUse("DepthTarget", FrameGraphAccess::Write, FrameGraphUsage::DepthStencilAttachment);
	useGraph.EndBackendPass();

	PassDesc storagePass;
	storagePass.name = "storage-test";
	storagePass.owner = "selftest";
	storagePass.reads.Push("StorageImage");
	storagePass.writes.Push("StorageImage");
	storagePass.uses.Push({ "StorageImage", FrameGraphAccess::ReadWrite, FrameGraphUsage::Storage });
	int storagePassIndex = useGraph.AddPass(storagePass);
	useGraph.BeginBackendPass(storagePassIndex);
	useGraph.ObserveBackendUse("StorageImage", FrameGraphAccess::ReadWrite, FrameGraphUsage::Storage);
	useGraph.EndBackendPass();

	PassDesc presentPass;
	presentPass.name = "present-test";
	presentPass.owner = "selftest";
	presentPass.reads.Push("PipelineImage[0]");
	presentPass.writes.Push("Backbuffer");
	presentPass.uses.Push({ "PipelineImage[0]", FrameGraphAccess::Read, FrameGraphUsage::Sampled });
	presentPass.uses.Push({ "Backbuffer", FrameGraphAccess::Write, FrameGraphUsage::Present });
	int presentPassIndex = useGraph.AddPass(presentPass);
	useGraph.BeginBackendPass(presentPassIndex);
	useGraph.ObserveBackendUse("PipelineImage[0]", FrameGraphAccess::Read, FrameGraphUsage::Sampled);
	useGraph.ObserveBackendUse("Backbuffer", FrameGraphAccess::Write, FrameGraphUsage::Present);
	useGraph.EndBackendPass();

	PassDesc readbackPass;
	readbackPass.name = "readback-test";
	readbackPass.owner = "selftest";
	readbackPass.reads.Push("Backbuffer");
	readbackPass.uses.Push({ "Backbuffer", FrameGraphAccess::Read, FrameGraphUsage::TransferSource });
	int readbackPassIndex = useGraph.AddPass(readbackPass);
	useGraph.BeginBackendPass(readbackPassIndex);
	useGraph.ObserveBackendUse("Backbuffer", FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
	useGraph.EndBackendPass();

	FString useReport;
	bool useOK = useGraph.Build(&useReport);

	FrameGraph aliasGraph;
	aliasGraph.DeclareAlias("PipelineImage[0]", "SceneColor");
	aliasGraph.AddPass({ "scene.target", "selftest", {}, { "SceneColor" } });
	aliasGraph.AddPass({ "postprocess", "selftest", { "PipelineImage[0]" }, { "PipelineImage[1]" } });
	FString aliasReport;
	bool aliasOK = aliasGraph.Build(&aliasReport) && aliasGraph.Order().Size() == 2;

	FrameGraph customGraph;
	customGraph.DeclareExternal("PipelineImage[0]");
	customGraph.DeclareExternal("CustomShader.example.texture");
	customGraph.AddPass({ "example", "Postprocess", { "PipelineImage[0]", "CustomShader.example.texture" }, { "PipelineImage[1]" } });
	FString customReport;
	bool customOK = customGraph.Build(&customReport) && customGraph.Order().Size() == 1;

	FrameGraph badGraph;
	badGraph.DeclareExternal("Input");
	PassDesc badPass;
	badPass.name = "bad-usage-test";
	badPass.owner = "selftest";
	badPass.reads.Push("Input");
	badPass.writes.Push("Output");
	badPass.uses.Push({ "NotInReadWriteLists", FrameGraphAccess::Read, FrameGraphUsage::Sampled });
	badGraph.AddPass(badPass);
	FString badReport;
	bool badDetected = !badGraph.Build(&badReport) && badReport.Len() > 0;

	Printf(ok && orderMatchesDeclaration && useOK && aliasOK && customOK && badDetected ? "selftest: PASS\n" : "selftest: FAIL\n");
}

// Real per-frame data: whatever GLPPRenderState::Draw()/VkPPRenderState::Draw()
// recorded via AddPass() since the last Graph().Reset() (once per frame, next to
// Resources().BeginFrame()). Covers tonemap/colormap/lens/fxaa (always nameable,
// via the special PPTextureType names) plus ssao/exposure/bloom/blur and the
// named shadowmap producer and custom shader passes with graph-only external
// texture inputs. Mirrors
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

	// The scene.target producer and, on Vulkan, scene.resolve now provide the
	// scene inputs. GL declares its non-MSAA SceneColor/PipelineImage[0] alias
	// when the target is selected. The remaining externals are genuine
	// persistent or engine-owned boundaries. PipelineImage[1] is not listed:
	// tonemap always writes it before anything reads it, in every real ordering,
	// so declaring it external would mask a genuine ordering bug.
	graph.DeclareExternal("EyeTexture[0]");
	graph.DeclareExternal("EyeTexture[1]");
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
