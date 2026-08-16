#pragma once

// Frame resource registry -- PHASE 1: it records, it does not allocate.
//
// Design and rationale: docs/frame-graph-resources.md. Analysis that motivated
// it: docs/frame-analysis.md, in particular section 3.3 -- resource lifetimes are
// managed per-effect by hand, so nothing knows the SET of live resources at a
// given frame size, and nothing can report, validate or alias them.
//
// Deliberately Metal-side for now. Nothing in this interface is Metal-specific
// except the pixel-format int, so lifting it to hwrenderer/ when a second backend
// registers anything is mechanical. Putting a shared header in hwrenderer/ with
// exactly one user would be the worse mistake -- see the open questions in the
// design doc.
//
// Why SizeRule exists, since it is the only non-obvious part: recording how a
// size is DERIVED, not just what it currently is, is what makes a mismatch
// checkable. AO's quarter-res buffer rounds scene heights 773 and 776 to the same
// 194 rows while the full-res consumers do not, and today no single place holds
// both numbers, so nobody can notice.

#include <stddef.h>
#include <vector>

#include "zstring.h"

struct MtSizeRule {
  enum Kind : unsigned char {
    Fixed,       // dimensions are what they are
    SceneFull,   // == scene size
    SceneScaled, // == ceil(scene / divisor)
  };
  Kind kind = Fixed;
  int divisor = 1;

  static MtSizeRule Full() { return {SceneFull, 1}; }
  static MtSizeRule Scaled(int d) { return {SceneScaled, d > 0 ? d : 1}; }
  static MtSizeRule Absolute() { return {Fixed, 1}; }
};

struct MtResourceDesc {
  const char *name = nullptr;  // stable, e.g. "SceneColor", "AO.Ambient"
  const char *owner = nullptr; // "MtRenderBuffers", "MtAOModule"
  int width = 0;
  int height = 0;
  int samples = 1;
  int pixelFormat = 0; // MTL::PixelFormat, kept as int so this header stays clean
  MtSizeRule size;
  bool transient = false; // dead at end of frame -- the phase-3 aliasing candidate
};

class MtFrameResources {
public:
  // Called where the texture is created today. Re-declaring a name with new
  // dimensions is the recreate path and is expected, not an error.
  void Declare(const MtResourceDesc &desc, const void *backendHandle);
  void Forget(const char *name);

  // Called where the resource is used. Cheap: stores the current frame index.
  // "Declared but untouched this frame" is the most valuable line in the dump --
  // it answers WHICH PATH RAN structurally, rather than depending on a log label
  // existing on that path, which is exactly what the compute AO path lacks.
  void Touch(const char *name);

  void BeginFrame(int sceneWidth, int sceneHeight);

  // Appends a human-readable table. Never throws, never asserts: this runs in a
  // shipping renderer and a false positive that kills the frame is worse than the
  // bug it catches.
  void Dump(FString &out) const;

  size_t TotalBytes() const;
  int SceneWidth() const { return mSceneWidth; }
  int SceneHeight() const { return mSceneHeight; }

private:
  struct Entry {
    MtResourceDesc desc;
    const void *handle = nullptr;
    unsigned long long lastTouchedFrame = 0;
    bool everTouched = false; // false = NO Touch call site, not 'unused'
    size_t bytes = 0;
  };

  Entry *Find(const char *name);
  const Entry *Find(const char *name) const;

  std::vector<Entry> mEntries;
  int mSceneWidth = 0;
  int mSceneHeight = 0;
  unsigned long long mFrame = 0;
};

// One registry per process. A singleton is the right call for phase 1: it keeps
// the diff at the registration sites down to one line and touches no class
// layout. When this moves to hwrenderer/ it should hang off the framebuffer.
MtFrameResources &MtResources();
