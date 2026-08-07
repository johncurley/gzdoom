// Orientation probe for MtPostprocess::BlitCurrentToImage's format-conversion
// branch.
//
// That branch has never executed. Its only caller, CreateWipeTexture, requests
// the pipeline format deliberately, so the raw copyFromTexture leg is always
// taken. But the branch is kept as a guarded future conversion path, and its
// V flip was derived from a bug report (dce604c31 shipped an upside-down screen
// wipe when mt_hdr_pipeline made the pipeline images RGBA16Float while
// CreateWipeTexture still asked for BGRA8Unorm) rather than observed. A comment
// in the source says as much and asks for verification.
//
// Verifying it by triggering a real wipe does not work: PerformWipe runs to
// completion inside a single D_Display call, so no per-frame hook can catch a
// frame mid-wipe, and a screenshot afterwards shows the destination screen.
//
// So this probe asks the question directly instead, and it is a question about
// two images rather than about a wipe:
//
//   A = BlitCurrentToImage into a texture in the PIPELINE format -> copy leg
//   B = BlitCurrentToImage into a texture in the OTHER format    -> convert leg
//
// The copy leg is an exact texel copy and is the behaviour wipes have today, so
// A is ground truth. B must match A's orientation. The two textures are
// different formats and different precisions, so they are compared by the shape
// of their vertical profile, not pixel for pixel: take each row's mean
// luminance and correlate B's profile against A's, and separately against A's
// REVERSED. Whichever correlates better is the orientation, and the margin
// between them says how much the answer is worth.
//
// The margin matters because a vertically symmetric image cannot answer this at
// all -- both correlations would be equal and high. That failure mode is
// reported rather than hidden, which is the thing this branch keeps relearning:
// a test whose two outcomes look the same is not a test.

#include "../system/mt_renderdevice.h"
#include "../system/mt_commandbuffer.h"
#include "mt_postprocess.h"
#include "mt_renderbuffers.h"
#include "../textures/mt_texture.h"
#include "c_dispatch.h"
#include "printf.h"
#include <Metal/Metal.hpp>
#include <vector>
#include <cmath>

static bool gWipeProbeArmed = false;
static int gWipeProbeFramesLeft = 0;

namespace {

// Reads a texture into per-row mean luminance. Only the profile is needed, so
// this deliberately does not reconstruct the image.
bool ReadRowProfile(MetalRenderDevice *fb, MTL::Texture *tex,
                    std::vector<double> &profile) {
  if (!tex)
    return false;

  const MTL::PixelFormat fmt = tex->pixelFormat();
  const bool halfFloat = fmt == MTL::PixelFormatRGBA16Float;
  const bool bgra8 = fmt == MTL::PixelFormatBGRA8Unorm;
  if (!halfFloat && !bgra8) {
    Printf(PRINT_HIGH, "  (wipe probe: unhandled pixel format %d)\n", (int)fmt);
    return false;
  }

  const int w = (int)tex->width();
  const int h = (int)tex->height();
  const size_t bytesPerRow = (size_t)w * (halfFloat ? 8 : 4);
  const size_t dataSize = bytesPerRow * (size_t)h;

  fb->GetCommands()->FlushCommands(true);

  MTL::Buffer *staging =
      fb->device->device->newBuffer(dataSize, MTL::ResourceStorageModeShared);
  if (!staging)
    return false;

  auto cmdBuf = fb->GetCommands()->GetBlitCommandBuffer();
  if (!cmdBuf) {
    staging->release();
    return false;
  }
  auto blit = cmdBuf->blitCommandEncoder();
  blit->copyFromTexture(tex, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1),
                        staging, 0, bytesPerRow, dataSize);
  blit->endEncoding();
  cmdBuf->commit();
  cmdBuf->waitUntilCompleted();
  cmdBuf->release();

  profile.assign((size_t)h, 0.0);
  for (int y = 0; y < h; ++y) {
    double sum = 0.0;
    if (halfFloat) {
      const __fp16 *px = (const __fp16 *)((const uint8_t *)staging->contents() +
                                          (size_t)y * bytesPerRow);
      for (int x = 0; x < w; ++x)
        sum += 0.299 * (float)px[x * 4 + 0] + 0.587 * (float)px[x * 4 + 1] +
               0.114 * (float)px[x * 4 + 2];
    } else {
      const uint8_t *px = (const uint8_t *)staging->contents() +
                          (size_t)y * bytesPerRow;
      // BGRA8
      for (int x = 0; x < w; ++x)
        sum += (0.299 * px[x * 4 + 2] + 0.587 * px[x * 4 + 1] +
                0.114 * px[x * 4 + 0]) / 255.0;
    }
    profile[y] = sum / (double)w;
  }
  staging->release();
  return true;
}

double Correlate(const std::vector<double> &a, const std::vector<double> &b) {
  const size_t n = a.size();
  if (n == 0 || b.size() != n)
    return 0.0;
  double ma = 0.0, mb = 0.0;
  for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
  ma /= (double)n; mb /= (double)n;
  double cov = 0.0, va = 0.0, vb = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double da = a[i] - ma, db = b[i] - mb;
    cov += da * db; va += da * da; vb += db * db;
  }
  if (va <= 0.0 || vb <= 0.0)
    return 0.0;
  return cov / std::sqrt(va * vb);
}

MTL::Texture *MakeTarget(MetalRenderDevice *fb, int w, int h,
                         MTL::PixelFormat format) {
  auto desc = MTL::TextureDescriptor::alloc()->init();
  desc->setWidth(w);
  desc->setHeight(h);
  desc->setPixelFormat(format);
  desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
  desc->setStorageMode(MTL::StorageModePrivate);
  MTL::Texture *tex = fb->device->device->newTexture(desc);
  desc->release();
  return tex;
}

} // namespace

void MtWipeProbeIfArmed(MetalRenderDevice *fb) {
  if (!gWipeProbeArmed || !fb)
    return;
  if (--gWipeProbeFramesLeft > 0)
    return;
  gWipeProbeArmed = false;

  auto buffers = fb->GetBuffers();
  auto pp = fb->GetPostprocess();
  if (!buffers || !pp) {
    Printf(PRINT_HIGH, "mt_wipe_probe: no buffers/postprocess.\n");
    return;
  }

  auto srcTex = buffers->PipelineImage[pp->mCurrentPipelineImage]->GetTexture();
  if (!srcTex) {
    Printf(PRINT_HIGH, "mt_wipe_probe: no pipeline image.\n");
    return;
  }

  const int w = (int)srcTex->width();
  const int h = (int)srcTex->height();
  const MTL::PixelFormat pipeFmt = (MTL::PixelFormat)buffers->GetPipelineFormat();
  // Deliberately the OTHER format, which is the entire point -- it is what
  // forces BlitCurrentToImage down the conversion branch.
  const MTL::PixelFormat otherFmt = pipeFmt == MTL::PixelFormatRGBA16Float
                                        ? MTL::PixelFormatBGRA8Unorm
                                        : MTL::PixelFormatRGBA16Float;

  Printf(PRINT_HIGH, "mt_wipe_probe: BlitCurrentToImage orientation, %dx%d\n", w, h);
  Printf(PRINT_HIGH, "  pipeline format %d (copy leg) vs %d (conversion leg)\n",
         (int)pipeFmt, (int)otherFmt);

  MTL::Texture *dstA = MakeTarget(fb, w, h, pipeFmt);
  MTL::Texture *dstB = MakeTarget(fb, w, h, otherFmt);
  if (!dstA || !dstB) {
    Printf(PRINT_HIGH, "  could not create probe targets.\n");
    if (dstA) dstA->release();
    if (dstB) dstB->release();
    return;
  }

  pp->BlitCurrentToImage(dstA);
  pp->BlitCurrentToImage(dstB);

  std::vector<double> pa, pb;
  if (!ReadRowProfile(fb, dstA, pa) || !ReadRowProfile(fb, dstB, pb)) {
    dstA->release();
    dstB->release();
    return;
  }

  std::vector<double> paRev(pa.rbegin(), pa.rend());
  const double rSame = Correlate(pb, pa);
  const double rFlip = Correlate(pb, paRev);

  // How much the source image can answer the question at all. A vertically
  // symmetric image correlates equally with itself and its reverse, and then
  // neither verdict below means anything.
  const double selfFlip = Correlate(pa, paRev);

  double meanA = 0.0, meanB = 0.0;
  for (double v : pa) meanA += v;
  for (double v : pb) meanB += v;
  meanA /= (double)pa.size();
  meanB /= (double)pb.size();

  Printf(PRINT_HIGH, "  mean row luminance   copy %.5f   convert %.5f\n", meanA, meanB);
  Printf(PRINT_HIGH, "  r(convert, copy)          = %+.4f\n", rSame);
  Printf(PRINT_HIGH, "  r(convert, copy REVERSED) = %+.4f\n", rFlip);
  Printf(PRINT_HIGH, "  r(copy, copy REVERSED)    = %+.4f   <- discriminating power;\n"
                     "                                        near +1 means the image is\n"
                     "                                        vertically symmetric and the\n"
                     "                                        verdict below is worthless\n",
         selfFlip);

  if (meanA <= 0.0 || meanB <= 0.0) {
    Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
           "  VERDICT: inconclusive -- one leg produced a black image, so nothing "
           "was measured.\n" TEXTCOLOR_NORMAL);
  } else if (std::fabs(rSame - rFlip) < 0.1) {
    Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
           "  VERDICT: inconclusive -- the two correlations are too close to "
           "separate.\n" TEXTCOLOR_NORMAL);
  } else if (rSame > rFlip) {
    Printf(PRINT_HIGH, TEXTCOLOR_GREEN
           "  VERDICT: AGREE. The conversion leg matches the copy leg's "
           "orientation.\n" TEXTCOLOR_NORMAL);
  } else {
    Printf(PRINT_HIGH, TEXTCOLOR_RED
           "  VERDICT: INVERTED. The conversion leg is upside down relative to "
           "the copy leg.\n" TEXTCOLOR_NORMAL);
  }

  dstA->release();
  dstB->release();
}

// Arming is allowed with no Metal device for the same reason mt_ao_probe allows
// it: command-line arming happens before the backend exists, and that is what
// makes an unattended capture launch possible.
CCMD(mt_wipe_probe) {
  if (screen && !screen->IsMetal())
    Printf(PRINT_HIGH, TEXTCOLOR_YELLOW
           "mt_wipe_probe: the Metal backend is not active; this will not "
           "fire.\n" TEXTCOLOR_NORMAL);

  gWipeProbeArmed = true;
  gWipeProbeFramesLeft = 10;
  if (argv.argc() > 1)
    gWipeProbeFramesLeft = atoi(argv[1]);
  Printf(PRINT_HIGH, "mt_wipe_probe: armed for %d frames.\n", gWipeProbeFramesLeft);
}
