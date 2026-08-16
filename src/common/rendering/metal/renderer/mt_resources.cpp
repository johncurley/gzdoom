// Frame resource registry, phase 1. See mt_resources.h and
// docs/frame-graph-resources.md.

#include "mt_resources.h"

#include <Metal/Metal.hpp>

#include "c_dispatch.h"
#include "printf.h"

#include <algorithm>
#include <string.h>

namespace {

// Bytes per pixel for the formats this renderer actually creates. Switching over
// the MTL constants rather than hardcoding their numeric values keeps this
// compiler-checked -- a renamed or removed constant fails the build instead of
// silently mis-reporting memory.
size_t BytesPerPixel(int format) {
  switch ((MTL::PixelFormat)format) {
  case MTL::PixelFormatR8Unorm:
  case MTL::PixelFormatA8Unorm:
  case MTL::PixelFormatStencil8:
    return 1;
  case MTL::PixelFormatR16Float:
  case MTL::PixelFormatRG8Unorm:
  case MTL::PixelFormatDepth16Unorm:
    return 2;
  case MTL::PixelFormatRGBA8Unorm:
  case MTL::PixelFormatRGBA8Unorm_sRGB:
  case MTL::PixelFormatBGRA8Unorm:
  case MTL::PixelFormatBGRA8Unorm_sRGB:
  case MTL::PixelFormatRG16Float:
  case MTL::PixelFormatR32Float:
  case MTL::PixelFormatDepth32Float:
  case MTL::PixelFormatRGB10A2Unorm:
  case MTL::PixelFormatRG11B10Float:
    return 4;
  case MTL::PixelFormatDepth24Unorm_Stencil8:
  case MTL::PixelFormatDepth32Float_Stencil8:
    return 5; // 4 + 1, near enough for a budget figure
  case MTL::PixelFormatRGBA16Float:
    return 8;
  case MTL::PixelFormatRGBA32Float:
    return 16;
  default:
    return 0; // unknown -- reported as such rather than guessed
  }
}

const char *FormatName(int format) {
  switch ((MTL::PixelFormat)format) {
  case MTL::PixelFormatR8Unorm: return "R8";
  case MTL::PixelFormatA8Unorm: return "A8";
  case MTL::PixelFormatStencil8: return "S8";
  case MTL::PixelFormatR16Float: return "R16F";
  case MTL::PixelFormatRG8Unorm: return "RG8";
  case MTL::PixelFormatRG16Float: return "RG16F";
  case MTL::PixelFormatR32Float: return "R32F";
  case MTL::PixelFormatRGBA8Unorm: return "RGBA8";
  case MTL::PixelFormatRGBA8Unorm_sRGB: return "RGBA8s";
  case MTL::PixelFormatBGRA8Unorm: return "BGRA8";
  case MTL::PixelFormatBGRA8Unorm_sRGB: return "BGRA8s";
  case MTL::PixelFormatRGBA16Float: return "RGBA16F";
  case MTL::PixelFormatRGBA32Float: return "RGBA32F";
  case MTL::PixelFormatRGB10A2Unorm: return "RGB10A2";
  case MTL::PixelFormatRG11B10Float: return "RG11B10F";
  case MTL::PixelFormatDepth16Unorm: return "D16";
  case MTL::PixelFormatDepth32Float: return "D32F";
  case MTL::PixelFormatDepth24Unorm_Stencil8: return "D24S8";
  case MTL::PixelFormatDepth32Float_Stencil8: return "D32FS8";
  default: return "?";
  }
}

const char *RuleName(const MtSizeRule &r) {
  switch (r.kind) {
  case MtSizeRule::SceneFull: return "scene";
  case MtSizeRule::SceneScaled: return "scene/N";
  default: return "fixed";
  }
}

int Ceil(int a, int b) { return b > 0 ? (a + b - 1) / b : a; }

} // namespace

MtFrameResources &MtResources() {
  static MtFrameResources instance;
  return instance;
}

MtFrameResources::Entry *MtFrameResources::Find(const char *name) {
  if (!name)
    return nullptr;
  for (auto &e : mEntries) {
    // Pointer equality first: every name in the tree is a string literal, so this
    // hits almost always and keeps Declare/Touch off strcmp.
    if (e.desc.name == name || (e.desc.name && strcmp(e.desc.name, name) == 0))
      return &e;
  }
  return nullptr;
}

const MtFrameResources::Entry *MtFrameResources::Find(const char *name) const {
  return const_cast<MtFrameResources *>(this)->Find(name);
}

void MtFrameResources::Declare(const MtResourceDesc &desc,
                               const void *backendHandle) {
  if (!desc.name)
    return;

  const size_t bpp = BytesPerPixel(desc.pixelFormat);
  const size_t bytes = bpp * (size_t)std::max(desc.width, 0) *
                       (size_t)std::max(desc.height, 0) *
                       (size_t)std::max(desc.samples, 1);

  if (Entry *existing = Find(desc.name)) {
    existing->desc = desc;
    existing->handle = backendHandle;
    existing->bytes = bytes;
    return;
  }

  Entry e;
  e.desc = desc;
  e.handle = backendHandle;
  e.bytes = bytes;
  mEntries.push_back(e);
}

void MtFrameResources::Forget(const char *name) {
  if (!name)
    return;
  mEntries.erase(std::remove_if(mEntries.begin(), mEntries.end(),
                                [name](const Entry &e) {
                                  return e.desc.name == name ||
                                         (e.desc.name &&
                                          strcmp(e.desc.name, name) == 0);
                                }),
                 mEntries.end());
}

void MtFrameResources::Touch(const char *name) {
  if (Entry *e = Find(name)) {
    e->lastTouchedFrame = mFrame;
    e->everTouched = true;
  }
}

void MtFrameResources::BeginFrame(int sceneWidth, int sceneHeight) {
  mFrame++;
  mSceneWidth = sceneWidth;
  mSceneHeight = sceneHeight;
}

size_t MtFrameResources::TotalBytes() const {
  size_t total = 0;
  for (const auto &e : mEntries)
    total += e.bytes;
  return total;
}

void MtFrameResources::Dump(FString &out) const {
  out.AppendFormat("scene %dx%d   %zu resources   %.1f MB   frame %llu\n\n",
                   mSceneWidth, mSceneHeight, mEntries.size(),
                   (double)TotalBytes() / (1024.0 * 1024.0), mFrame);
  out.AppendFormat("  %-22s %-16s %-11s %-8s %-8s %-7s %s\n", "name", "owner",
                   "size", "format", "rule", "touched", "MB");

  for (const auto &e : mEntries) {
    const auto &d = e.desc;
    FString rule = RuleName(d.size);
    if (d.size.kind == MtSizeRule::SceneScaled)
      rule.Format("scene/%d", d.size.divisor);

    // Does the recorded size still match what its rule says it should be? A
    // mismatch means someone's EnsureTextures did not run for the current scene
    // size. Reported, never fatal.
    FString flag;
    if (d.size.kind == MtSizeRule::SceneFull && mSceneWidth > 0 &&
        (d.width != mSceneWidth || d.height != mSceneHeight)) {
      flag.Format("  <- STALE, rule says %dx%d", mSceneWidth, mSceneHeight);
    } else if (d.size.kind == MtSizeRule::SceneScaled && mSceneWidth > 0) {
      const int ew = Ceil(mSceneWidth, d.size.divisor);
      const int eh = Ceil(mSceneHeight, d.size.divisor);
      if (d.width != ew || d.height != eh)
        flag.Format("  <- STALE, rule says %dx%d", ew, eh);
    }

    FString size;
    size.Format("%dx%d", d.width, d.height);
    // Three states, not two. A resource with no Touch call site anywhere reads
    // "n/i" (not instrumented) rather than "-", because reporting it as unused
    // would be a finding manufactured by missing instrumentation -- which is
    // exactly the false signal this registry exists to remove.
    const bool touched = e.lastTouchedFrame == mFrame && mFrame != 0;
    const char *touchState = touched ? "yes" : (e.everTouched ? "-" : "n/i");

    out.AppendFormat("  %-22s %-16s %-11s %-8s %-8s %-7s %5.2f%s%s\n",
                     d.name ? d.name : "?", d.owner ? d.owner : "?",
                     size.GetChars(), FormatName(d.pixelFormat),
                     rule.GetChars(), touchState,
                     (double)e.bytes / (1024.0 * 1024.0),
                     d.transient ? "  transient" : "", flag.GetChars());
  }

  // The line this whole registry exists for. A resource that is declared but was
  // not used this frame means a path did not run -- a disabled feature holding
  // memory, or an algorithm variant nobody selected.
  FString untouched;
  for (const auto &e : mEntries) {
    if (mFrame != 0 && e.everTouched && e.lastTouchedFrame != mFrame) {
      if (untouched.Len())
        untouched += ", ";
      untouched += e.desc.name ? e.desc.name : "?";
    }
  }
  if (untouched.Len())
    out.AppendFormat("\n  UNTOUCHED this frame: %s\n", untouched.GetChars());
}

CCMD(mt_resources) {
  FString out;
  MtResources().Dump(out);
  Printf(PRINT_HIGH, "%s", out.GetChars());
}
