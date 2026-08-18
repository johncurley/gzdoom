/*
**  Frame graph resource registry -- phase 1 (record + report, no allocation)
**  See docs/frame-graph-resources.md for the design this implements.
*/

#include <cstring>
#include "hwrenderer/frame/hw_resources.h"
#include "zstring.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_video.h"

CVAR(Bool, r_resource_validate, false, 0)

FrameResources::Entry *FrameResources::Find(const char *name)
{
	for (auto &entry : mEntries)
	{
		if (entry.desc.name == name || strcmp(entry.desc.name, name) == 0)
			return &entry;
	}
	return nullptr;
}

const FrameResources::Entry *FrameResources::Find(const char *name) const
{
	return const_cast<FrameResources *>(this)->Find(name);
}

void FrameResources::Declare(const ResourceDesc &desc, void *backendHandle)
{
	Entry *entry = Find(desc.name);
	if (!entry)
	{
		entry = &mEntries[mEntries.Push({})];
	}
	entry->desc = desc;
	entry->backendHandle = backendHandle;
	entry->declaredFrame = mFrameCounter;
}

void FrameResources::Forget(const char *name)
{
	for (unsigned int i = 0; i < mEntries.Size(); i++)
	{
		if (strcmp(mEntries[i].desc.name, name) == 0)
		{
			mEntries.Delete(i);
			return;
		}
	}
}

void FrameResources::Touch(const char *name, bool asOutput)
{
	Entry *entry = Find(name);
	if (!entry)
		return;
	if (asOutput)
		entry->lastWriteFrame = mFrameCounter;
	else
		entry->lastReadFrame = mFrameCounter;
}

void FrameResources::BeginFrame(int sceneWidth, int sceneHeight)
{
	mFrameCounter++;
	mSceneWidth = sceneWidth;
	mSceneHeight = sceneHeight;
}

size_t FrameResources::BytesPerPixel(ResourceFormat format)
{
	switch (format)
	{
	case ResourceFormat::RGBA8:   return 4;
	case ResourceFormat::RGBA16F: return 8;
	case ResourceFormat::R8:      return 1;
	case ResourceFormat::RG16F:   return 4;
	case ResourceFormat::R32F:    return 4;
	case ResourceFormat::D24S8:   return 4;
	default:                      return 0;
	}
}

const char *FrameResources::FormatName(ResourceFormat format)
{
	switch (format)
	{
	case ResourceFormat::RGBA8:   return "RGBA8";
	case ResourceFormat::RGBA16F: return "RGBA16F";
	case ResourceFormat::R8:      return "R8";
	case ResourceFormat::RG16F:   return "RG16F";
	case ResourceFormat::R32F:    return "R32F";
	case ResourceFormat::D24S8:   return "D24S8";
	default:                      return "Unknown";
	}
}

size_t FrameResources::TotalBytes() const
{
	size_t total = 0;
	for (auto &entry : mEntries)
	{
		total += (size_t)entry.desc.width * (size_t)entry.desc.height
			* (size_t)entry.desc.samples * BytesPerPixel(entry.desc.format);
	}
	return total;
}

void FrameResources::ValidateFrame(FString *report) const
{
	*report = "";
	for (auto &entry : mEntries)
	{
		const SizeRule &rule = entry.desc.size;
		int expectedWidth = entry.desc.width, expectedHeight = entry.desc.height;
		bool checkable = true;
		switch (rule.kind)
		{
		case SizeRule::SceneFull:
			expectedWidth = mSceneWidth;
			expectedHeight = mSceneHeight;
			break;
		case SizeRule::SceneScaled:
			expectedWidth = rule.divisor > 0 ? (mSceneWidth + rule.divisor - 1) / rule.divisor : mSceneWidth;
			expectedHeight = rule.divisor > 0 ? (mSceneHeight + rule.divisor - 1) / rule.divisor : mSceneHeight;
			break;
		case SizeRule::MipOf:
		case SizeRule::Fixed:
		default:
			checkable = false;
			break;
		}
		if (checkable && (entry.desc.width != expectedWidth || entry.desc.height != expectedHeight))
		{
			report->AppendFormat("stale size: %s is %dx%d, rule expects %dx%d\n",
				entry.desc.name, entry.desc.width, entry.desc.height, expectedWidth, expectedHeight);
		}

		if (entry.declaredFrame < mFrameCounter && entry.lastWriteFrame < mFrameCounter && entry.lastReadFrame < mFrameCounter)
		{
			report->AppendFormat("untouched this frame: %s (owner %s)\n", entry.desc.name, entry.desc.owner);
		}
	}
}

void FrameResources::Dump(FString *out) const
{
	*out = "";
	out->AppendFormat("scene %dx%d   %u resources   %.1f MB\n",
		mSceneWidth, mSceneHeight, mEntries.Size(), TotalBytes() / (1024.0 * 1024.0));
	out->AppendFormat("\n  %-22s%-19s%-12s%-9s%-4s%-4s  MB\n", "name", "owner", "size", "format", "w", "r");

	TArray<FString> untouched;
	for (auto &entry : mEntries)
	{
		FString size;
		size.Format("%dx%d", entry.desc.width, entry.desc.height);
		bool written = entry.lastWriteFrame == mFrameCounter;
		bool read = entry.lastReadFrame == mFrameCounter;
		double mb = (double)entry.desc.width * entry.desc.height * entry.desc.samples
			* BytesPerPixel(entry.desc.format) / (1024.0 * 1024.0);

		out->AppendFormat("  %-22s%-19s%-12s%-9s%-4s%-4s  %.1f\n",
			entry.desc.name, entry.desc.owner, size.GetChars(), FormatName(entry.desc.format),
			written ? "w" : "-", read ? "r" : "-", mb);

		if (!written && !read)
			untouched.Push(entry.desc.name);
	}

	if (untouched.Size() > 0)
	{
		out->AppendFormat("\n  UNTOUCHED this frame:");
		for (unsigned int i = 0; i < untouched.Size(); i++)
			out->AppendFormat("%s%s", i ? ", " : " ", untouched[i].GetChars());
		out->AppendFormat("\n");
	}
}

CCMD(r_resources)
{
	if (!screen)
	{
		Printf("No render backend active.\n");
		return;
	}
	FString out;
	screen->Resources().Dump(&out);
	Printf("%s", out.GetChars());

	if (r_resource_validate)
	{
		FString report;
		screen->Resources().ValidateFrame(&report);
		if (report.Len() > 0)
			Printf("\n%s", report.GetChars());
	}
}
