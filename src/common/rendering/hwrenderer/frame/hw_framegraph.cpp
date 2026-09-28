/*
**  Frame graph -- pass description, dependency graph, topological order
**  See hw_framegraph.h for the versioning model this implements.
*/

#include <cstring>
#include <utility>
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

static const char *PreparationName(FrameGraphPreparation preparation)
{
	return preparation == FrameGraphPreparation::Worker ? "worker" : "render-thread";
}

void FrameGraph::Reset()
{
	// Uploads can be recorded during startup/precache, before the first graph
	// frame begins. Keep them until a sampled read is observed; retire consumed
	// records now that the frame in which that read happened is ending.
	for (unsigned int i = 0; i < mUploads.Size();)
	{
		const uint64_t *readSequence = mResourceReads.CheckKey(mUploads[i].resource);
		if (readSequence && *readSequence > mUploads[i].sequence)
			mUploads.Delete(i);
		else
			++i;
	}

	mPasses.Clear();
	mExternals.Clear();
	mOutputs.Clear();
	mTransientResources.Clear();
	mAliases.Clear();
	mEdges.Clear();
	mOrder.Clear();
	mDeadPassCandidates.Clear();
	mLifetimes.Clear();
	mBackendObserved.Clear();
	mObservedUses.Clear();
	mOwnedSceneReadNames.Clear();
	mResourceReads.Clear();
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
	if (!name)
		return;
	for (const char *external : mExternals)
		if (NameEq(external, name))
			return;
	mExternals.Push(name);
}

void FrameGraph::DeclareOutput(const char *name)
{
	if (!name)
		return;
	for (const char *output : mOutputs)
		if (NameEq(output, name))
			return;
	mOutputs.Push(name);
}

void FrameGraph::DeclareTransient(const char *name)
{
	if (!name)
		return;
	for (const char *resource : mTransientResources)
		if (NameEq(resource, name))
			return;
	mTransientResources.Push(name);
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

void FrameGraph::RecordUpload(const FrameGraphUploadDesc &desc)
{
	UploadObservation upload;
	if (desc.resource)
		upload.resource = desc.resource;
	if (desc.owner)
		upload.owner = desc.owner;
	upload.preparation = desc.preparation;
	upload.stagingRetained = desc.stagingRetained;
	upload.transferRecorded = desc.transferRecorded;
	upload.orderedBeforeConsumers = desc.orderedBeforeConsumers;
	upload.sequence = ++mObservationSequence;
	mUploads.Push(std::move(upload));
}

void FrameGraph::ObserveResourceRead(const char *name)
{
	if (!name)
		return;

	uint64_t sequence = ++mObservationSequence;
	uint64_t *lastSequence = mResourceReads.CheckKey(FString(name));
	if (lastSequence)
		*lastSequence = sequence;
	else
		mResourceReads.Insert(FString(name), sequence);
}

void FrameGraph::ObserveSceneMaterialRead(const char *name)
{
	if (!name)
		return;

	// Keep the existing upload-order observation for reads outside a scene
	// scope too; only the dependency declaration below is scene-specific.
	ObserveResourceRead(name);
	if (mActivePass < 0 || mActivePass >= (int)mPasses.Size())
		return;

	PassDesc &pass = mPasses[mActivePass];
	if (!pass.name || strncmp(pass.name, "scene.", 6) != 0)
		return;

	const char *canonicalName = CanonicalName(name);
	bool hasEarlierWriter = false;
	for (int passIndex = 0; passIndex < mActivePass && !hasEarlierWriter; passIndex++)
	{
		for (const char *written : mPasses[passIndex].writes)
		{
			if (NameEq(CanonicalName(written), canonicalName))
			{
				hasEarlierWriter = true;
				break;
			}
		}
	}

	const char *graphName = nullptr;
	for (const char *read : pass.reads)
	{
		if (NameEq(CanonicalName(read), canonicalName))
			graphName = read;
	}
	if (!graphName)
	{
		mOwnedSceneReadNames.Push(FString(name));
		graphName = mOwnedSceneReadNames[mOwnedSceneReadNames.Size() - 1].GetChars();
		pass.reads.Push(graphName);
	}

	if (!hasEarlierWriter)
		DeclareExternal(graphName);

	bool hasSampledRead = false;
	for (const ResourceUse &use : pass.uses)
	{
		if (use.access == FrameGraphAccess::Read && use.usage == FrameGraphUsage::Sampled &&
			NameEq(CanonicalName(use.name), canonicalName))
		{
			hasSampledRead = true;
			break;
		}
	}
	if (!hasSampledRead)
		pass.uses.Push({ graphName, FrameGraphAccess::Read, FrameGraphUsage::Sampled });

	bool alreadyObserved = false;
	for (const ObservedUse &observed : mObservedUses)
	{
		if (observed.pass == mActivePass && observed.use.access == FrameGraphAccess::Read &&
			observed.use.usage == FrameGraphUsage::Sampled &&
			NameEq(CanonicalName(observed.use.name), canonicalName))
		{
			alreadyObserved = true;
			break;
		}
	}
	if (!alreadyObserved)
		mObservedUses.Push({ mActivePass, { graphName, FrameGraphAccess::Read, FrameGraphUsage::Sampled } });
	mBackendObserved[mActivePass] = 1;
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
	mDeadPassCandidates.Clear();
	mLifetimes.Clear();
	ValidateUses(report);
	ValidateUploads(report);
	BuildEdges(report);
	bool ok = TopoSort(report);
	if (ok)
	{
		AnalyzeLiveness(report);
		AnalyzeLifetimes();
	}
	return ok && report->Len() == 0;
}

void FrameGraph::AnalyzeLiveness(FString *report)
{
	mDeadPassCandidates.Clear();
	TArray<uint8_t> live;
	live.Resize(mPasses.Size());
	for (unsigned int i = 0; i < live.Size(); i++)
		live[i] = 0;

	TArray<int> pending;
	for (const char *output : mOutputs)
	{
		int writer = -1;
		const char *canonicalOutput = CanonicalName(output);
		for (int passIndex = 0; passIndex < (int)mPasses.Size(); passIndex++)
		{
			for (const char *written : mPasses[passIndex].writes)
			{
				if (NameEq(CanonicalName(written), canonicalOutput))
					writer = passIndex;
			}
		}
		if (writer < 0)
			report->AppendFormat("required output '%s' has no producer in this frame\n", output);
		else
			pending.Push(writer);
	}

	for (int i = 0; i < (int)mPasses.Size(); i++)
		if (mPasses[i].keepAlive)
			pending.Push(i);

	while (pending.Size() > 0)
	{
		int passIndex = pending[pending.Size() - 1];
		pending.Delete(pending.Size() - 1);
		if (live[passIndex])
			continue;
		live[passIndex] = 1;
		for (const Edge &edge : mEdges)
			if (edge.to == passIndex)
				pending.Push(edge.from);
	}

	for (int i = 0; i < (int)mPasses.Size(); i++)
		if (!live[i])
			mDeadPassCandidates.Push(i);
}

void FrameGraph::AnalyzeLifetimes()
{
	mLifetimes.Clear();
	for (int order = 0; order < (int)mOrder.Size(); order++)
	{
		const PassDesc &pass = mPasses[mOrder[order]];
			auto recordUse = [&](const char *name)
		{
			if (!name)
				return;
			const char *canonical = CanonicalName(name);
			for (const char *external : mExternals)
				if (NameEq(CanonicalName(external), canonical))
					return;
			bool transient = false;
			for (const char *resource : mTransientResources)
				if (NameEq(CanonicalName(resource), canonical))
				{
					transient = true;
					break;
				}
			if (!transient)
				return;

			for (FrameGraphLifetime &lifetime : mLifetimes)
			{
				if (NameEq(lifetime.resource, canonical))
				{
					lifetime.lastOrder = order;
					return;
				}
			}
			mLifetimes.Push({ canonical, order, order });
		};
		for (const char *name : pass.reads)
			recordUse(name);
		for (const char *name : pass.writes)
			recordUse(name);
	}
}

void FrameGraph::ValidateUploads(FString *report) const
{
	for (const UploadObservation &upload : mUploads)
	{
		const char *resource = upload.resource.GetChars();
		if (!resource || !resource[0])
		{
			report->AppendFormat("upload (%s) has no resource name\n",
				upload.owner.GetChars());
			continue;
		}
		if (!upload.stagingRetained)
			report->AppendFormat("upload '%s' (%s) did not keep its source valid through transfer consumption\n",
				resource, upload.owner.GetChars());
		if (!upload.transferRecorded)
			report->AppendFormat("upload '%s' (%s) did not record its transfer\n",
				resource, upload.owner.GetChars());
		if (!upload.orderedBeforeConsumers)
			report->AppendFormat("upload '%s' (%s) is not ordered before consumer reads\n",
				resource, upload.owner.GetChars());
	}
}

void FrameGraph::Dump(FString *out) const
{
	*out = "";
	out->AppendFormat("%u passes, %u edges\n\n", mPasses.Size(), mEdges.Size());
	out->AppendFormat("  outputs:");
	if (mOutputs.Size() == 0)
		out->AppendFormat(" none\n");
	else
	{
		for (const char *output : mOutputs)
			out->AppendFormat(" %s", output);
		out->AppendFormat("\n");
	}

	out->AppendFormat("  order  pass                 owner            reads -> writes\n");
	for (int idx : mOrder)
	{
		const PassDesc &pass = mPasses[idx];
		FString reads, writes;
		for (unsigned int i = 0; i < pass.reads.Size(); i++)
			reads.AppendFormat("%s%s", i ? ", " : "", pass.reads[i]);
		for (unsigned int i = 0; i < pass.writes.Size(); i++)
			writes.AppendFormat("%s%s", i ? ", " : "", pass.writes[i]);

		out->AppendFormat("  %-7d%-21s%-17s%s -> %s%s\n",
			idx, pass.name, pass.owner, reads.GetChars(), writes.GetChars(),
			pass.keepAlive ? " [keep-alive]" : "");
		for (const ResourceUse &use : pass.uses)
			out->AppendFormat("           use: %-12s %-18s %s\n", AccessName(use.access), UsageName(use.usage), use.name);
	}

	out->AppendFormat("\n  dead-pass candidates:");
	if (mDeadPassCandidates.Size() == 0)
		out->AppendFormat(" none\n");
	else
	{
		for (int passIndex : mDeadPassCandidates)
			out->AppendFormat(" %s", mPasses[passIndex].name);
		out->AppendFormat("\n");
	}

	out->AppendFormat("\n  transient lifetimes (topological order):\n");
	if (mLifetimes.Size() == 0)
		out->AppendFormat("    none\n");
	for (const FrameGraphLifetime &lifetime : mLifetimes)
	{
		const char *first = mPasses[mOrder[lifetime.firstOrder]].name;
		const char *last = mPasses[mOrder[lifetime.lastOrder]].name;
		out->AppendFormat("    %-28s %d (%s) .. %d (%s)\n",
			lifetime.resource, lifetime.firstOrder, first,
			lifetime.lastOrder, last);
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

	if (mUploads.Size() > 0)
	{
		out->AppendFormat("\n  uploads:\n");
		for (const UploadObservation &upload : mUploads)
		{
			const char *resource = upload.resource.GetChars();
			const uint64_t *readSequence = mResourceReads.CheckKey(upload.resource);
			bool readAfter = readSequence && *readSequence > upload.sequence;
			out->AppendFormat("    %-28s owner=%s prepared=%s staging=%s transfer=%s ordered-before-read=%s read-after-upload=%s\n",
				resource, upload.owner.GetChars(), PreparationName(upload.preparation),
				upload.stagingRetained ? "yes" : "no",
				upload.transferRecorded ? "yes" : "no",
				upload.orderedBeforeConsumers ? "yes" : "no",
				readAfter ? "observed" : "not-observed");
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
	graph.DeclareOutput("PipelineImage[0]");
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

	FrameGraph uploadGraph;
	uploadGraph.RecordUpload({ "MetalTexture.test", "selftest",
		FrameGraphPreparation::Worker, true, true, true });
	uploadGraph.Reset(); // startup/precache upload predates the first graph frame
	uploadGraph.ObserveResourceRead("MetalTexture.test");
	FString uploadReport;
	bool uploadOK = uploadGraph.Build(&uploadReport) && uploadReport.Len() == 0;
	FString uploadDump;
	uploadGraph.Dump(&uploadDump);
	uploadOK = uploadOK &&
		strstr(uploadDump.GetChars(), "read-after-upload=observed") != nullptr;
	uploadGraph.Reset();
	FString retiredUploadDump;
	uploadGraph.Dump(&retiredUploadDump);
	uploadOK = uploadOK &&
		strstr(retiredUploadDump.GetChars(), "  uploads:") == nullptr;

	FrameGraph badUploadGraph;
	badUploadGraph.RecordUpload({ "MetalTexture.test", "selftest",
		FrameGraphPreparation::Worker, true, true, false });
	badUploadGraph.Reset();
	badUploadGraph.ObserveResourceRead("MetalTexture.test");
	FString badUploadReport;
	bool badUploadDetected = !badUploadGraph.Build(&badUploadReport) &&
		strstr(badUploadReport.GetChars(), "not ordered before consumer reads") != nullptr;

	FrameGraph livenessGraph;
	livenessGraph.DeclareExternal("Input");
	livenessGraph.DeclareTransient("Input"); // imported inputs never get transient lifetimes
	livenessGraph.DeclareTransient("Scratch");
	PassDesc producer;
	producer.name = "producer";
	producer.owner = "selftest";
	producer.writes.Push("Scratch");
	livenessGraph.AddPass(producer);
	PassDesc consumer;
	consumer.name = "consumer";
	consumer.owner = "selftest";
	consumer.reads.Push("Scratch");
	consumer.writes.Push("Color");
	livenessGraph.AddPass(consumer);
	PassDesc readback;
	readback.name = "screenshot.readback";
	readback.owner = "selftest";
	readback.reads.Push("Color");
	readback.keepAlive = true;
	livenessGraph.AddPass(readback);
	PassDesc unused;
	unused.name = "unused";
	unused.owner = "selftest";
	unused.reads.Push("Input");
	unused.writes.Push("Unused");
	livenessGraph.AddPass(unused);
	livenessGraph.DeclareOutput("Color");
	FString livenessReport;
	bool livenessOK = livenessGraph.Build(&livenessReport) &&
		livenessGraph.DeadPassCandidates().Size() == 1 &&
		strcmp(livenessGraph.Pass(livenessGraph.DeadPassCandidates()[0]).name, "unused") == 0 &&
		livenessGraph.Lifetimes().Size() == 1 &&
		strcmp(livenessGraph.Lifetimes()[0].resource, "Scratch") == 0 &&
		livenessGraph.Lifetimes()[0].firstOrder == 0 &&
		livenessGraph.Lifetimes()[0].lastOrder == 1;

	FrameGraph missingOutputGraph;
	missingOutputGraph.DeclareOutput("Missing");
	FString missingOutputReport;
	bool missingOutputDetected = !missingOutputGraph.Build(&missingOutputReport) &&
		strstr(missingOutputReport.GetChars(), "has no producer") != nullptr;

	FrameGraph wipeGraph;
	PassDesc wipeCapture;
	wipeCapture.name = "wipe.copy";
	wipeCapture.owner = "selftest";
	wipeCapture.writes.Push("WipeStartScreen");
	wipeCapture.keepAlive = true;
	wipeGraph.AddPass(wipeCapture);
	FString wipeReport;
	bool wipeOK = wipeGraph.Build(&wipeReport) && wipeGraph.DeadPassCandidates().Size() == 0;

	Printf(ok && orderMatchesDeclaration && useOK && aliasOK && customOK && badDetected &&
		uploadOK && badUploadDetected && livenessOK && missingOutputDetected && wipeOK ?
		"selftest: PASS\n" : "selftest: FAIL\n");
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
	TArray<const char *> transientNames;
	screen->Resources().GetTransientNames(transientNames);
	for (const char *name : transientNames)
		graph.DeclareTransient(name);

	FString report;
	bool ok = graph.Build(&report);

	FString dump;
	graph.Dump(&dump);
	Printf("%s\n", dump.GetChars());

	if (!ok && report.Len() > 0)
		Printf("Build() reported:\n%s\n", report.GetChars());
}
