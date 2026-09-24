/*
** texture.cpp
** The base texture class
**
**---------------------------------------------------------------------------
** Copyright 2004-2007 Randy Heit
** Copyright 2006-2018 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include "printf.h"
#include "files.h"
#include "filesystem.h"

#include "textures.h"
#include "bitmap.h"
#include "colormatcher.h"
#include "c_dispatch.h"
#include "m_fixed.h"
#include "imagehelpers.h"
#include "image.h"
#include "formats/multipatchtexture.h"
#include "gametexture.h"
#include "texturemanager.h"
#include "c_cvars.h"
#include "imagehelpers.h"
#include "v_video.h"
#include "v_font.h"
#include "r_translate.h"

EXTERN_CVAR(Int, gl_texture_hqresizemode)
EXTERN_CVAR(Int, gl_texture_hqresizemult)

// Wrappers to keep the definitions of these classes out of here.
IHardwareTexture* CreateHardwareTexture(int numchannels);

// Make sprite offset adjustment user-configurable per renderer.
int r_spriteadjustSW, r_spriteadjustHW;

//==========================================================================
//
// 
//
//==========================================================================

FTexture::FTexture (int lumpnum)
	:  SourceLump(lumpnum), bHasCanvas(false)
{
	bTranslucent = -1;
}

//===========================================================================
//
// FTexture::GetBgraBitmap
//
// Default returns just an empty bitmap. This needs to be overridden by
// any subclass that actually does return a software pixel buffer.
//
//===========================================================================

FBitmap FTexture::GetBgraBitmap(const PalEntry* remap, int* ptrans)
{
	FBitmap bmp;
	bmp.Create(Width, Height);
	return bmp;
}

//====================================================================
//
// CheckRealHeight
//
// Checks the posts in a texture and returns the lowest row (plus one)
// of the texture that is actually used.
//
//====================================================================

int FTexture::CheckRealHeight()
{
	auto pixels = Get8BitPixels(false);

	for(int h = GetHeight()-1; h>= 0; h--)
	{
		for(int w = 0; w < GetWidth(); w++)
		{
			if (pixels[h + w * GetHeight()] != 0)
			{
				return h;
			}
		}
	}
	return 0;
}

//===========================================================================
// 
//	Finds gaps in the texture which can be skipped by the renderer
//  This was mainly added to speed up one area in E4M6 of 007LTSD
//
//===========================================================================

bool FTexture::FindHoles(const unsigned char* buffer, int w, int h)
{
	const unsigned char* li;
	int y, x;
	int startdraw, lendraw;
	int gaps[5][2];
	int gapc = 0;


	// already done!
	if (areacount) return false;
	areacount = -1;	//whatever happens next, it shouldn't be done twice!

							// large textures and non-images are excluded for performance reasons
	if (h>512 || !GetImage()) return false;

	startdraw = -1;
	lendraw = 0;
	for (y = 0; y < h; y++)
	{
		li = buffer + w * y * 4 + 3;

		for (x = 0; x < w; x++, li += 4)
		{
			if (*li != 0) break;
		}

		if (x != w)
		{
			// non - transparent
			if (startdraw == -1)
			{
				startdraw = y;
				// merge transparent gaps of less than 16 pixels into the last drawing block
				if (gapc && y <= gaps[gapc - 1][0] + gaps[gapc - 1][1] + 16)
				{
					gapc--;
					startdraw = gaps[gapc][0];
					lendraw = y - startdraw;
				}
				if (gapc == 4) return false;	// too many splits - this isn't worth it
			}
			lendraw++;
		}
		else if (startdraw != -1)
		{
			if (lendraw == 1) lendraw = 2;
			gaps[gapc][0] = startdraw;
			gaps[gapc][1] = lendraw;
			gapc++;

			startdraw = -1;
			lendraw = 0;
		}
	}
	if (startdraw != -1)
	{
		gaps[gapc][0] = startdraw;
		gaps[gapc][1] = lendraw;
		gapc++;
	}
	if (startdraw == 0 && lendraw == h) return false;	// nothing saved so don't create a split list

	if (gapc > 0)
	{
		FloatRect* rcs = (FloatRect*)ImageArena.Alloc(gapc * sizeof(FloatRect));	// allocate this on the image arena

		for (x = 0; x < gapc; x++)
		{
			// gaps are stored as texture (u/v) coordinates
			rcs[x].width = rcs[x].left = -1.0f;
			rcs[x].top = (float)gaps[x][0] / (float)h;
			rcs[x].height = (float)gaps[x][1] / (float)h;
		}
		areas = rcs;
	}
	else areas = nullptr;
	areacount = gapc;

	return true;
}

//----------------------------------------------------------------------------
//
//
//
//----------------------------------------------------------------------------

void FTexture::CheckTrans(unsigned char* buffer, int size, int trans)
{
	if (bTranslucent == -1)
	{
		bTranslucent = trans;
		if (trans == -1)
		{
			uint32_t* dwbuf = (uint32_t*)buffer;
			for (int i = 0; i < size; i++)
			{
				uint32_t alpha = dwbuf[i] >> 24;

				if (alpha != 0xff && alpha != 0)
				{
					bTranslucent = 1;
					return;
				}
			}
			bTranslucent = 0;
		}
	}
}


//===========================================================================
// 
// smooth the edges of transparent fields in the texture
//
//===========================================================================

#ifdef WORDS_BIGENDIAN
#define MSB 0
#define SOME_MASK 0xffffff00
#else
#define MSB 3
#define SOME_MASK 0x00ffffff
#endif

#define CHKPIX(ofs) (l1[(ofs)*4+MSB]==255 ? (( ((uint32_t*)l1)[0] = ((uint32_t*)l1)[ofs]&SOME_MASK), trans=true ) : false)

bool FTexture::SmoothEdges(unsigned char* buffer, int w, int h)
{
	int x, y;
	bool trans = buffer[MSB] == 0; // If I set this to false here the code won't detect textures 
								   // that only contain transparent pixels.
	bool semitrans = false;
	unsigned char* l1;

	if (h <= 1 || w <= 1) return false;  // makes (a) no sense and (b) doesn't work with this code!

	l1 = buffer;


	if (l1[MSB] == 0 && !CHKPIX(1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-1) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-1)) CHKPIX(w);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;

	for (y = 1; y < h - 1; y++)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
		for (x = 1; x < w - 1; x++, l1 += 4)
		{
			if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1) && !CHKPIX(1) && !CHKPIX(-w - 1) && !CHKPIX(-w + 1) && !CHKPIX(w - 1) && !CHKPIX(w + 1)) CHKPIX(w);
			else if (l1[MSB] < 255) semitrans = true;
		}
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(w);
		else if (l1[MSB] < 255) semitrans = true;
		l1 += 4;
	}

	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(1);
	else if (l1[MSB] < 255) semitrans = true;
	l1 += 4;
	for (x = 1; x < w - 1; x++, l1 += 4)
	{
		if (l1[MSB] == 0 && !CHKPIX(-w) && !CHKPIX(-1)) CHKPIX(1);
		else if (l1[MSB] < 255) semitrans = true;
	}
	if (l1[MSB] == 0 && !CHKPIX(-w)) CHKPIX(-1);
	else if (l1[MSB] < 255) semitrans = true;

	return trans || semitrans;
}

//===========================================================================
// 
// Post-process the texture data after the buffer has been created
//
//===========================================================================

bool FTexture::ProcessData(unsigned char* buffer, int w, int h, bool ispatch)
{
	if (Masked)
	{
		Masked = SmoothEdges(buffer, w, h);
		if (Masked && !ispatch) FindHoles(buffer, w, h);
	}
	return true;
}

//===========================================================================
// 
//	Initializes the buffer for the texture data
//
//===========================================================================

FTextureBuffer FTexture::CreateTexBuffer(int translation, int flags)
{
	FTextureBuffer result;
	if (flags & CTF_Indexed)
	{
		// Indexed textures will never be translated and never be scaled.
		int w = GetWidth(), h = GetHeight();

		auto store = Get8BitPixels(false);
		const uint8_t* p = store.Data();

		result.mBuffer = new uint8_t[w * h];
		result.mWidth = w;
		result.mHeight = h;
		result.mContentId = 0;
		ImageHelpers::FlipNonSquareBlock(result.mBuffer, p, h, w, h);
	}
	else
	{
		unsigned char* buffer = nullptr;
		int W, H;
		int isTransparent = -1;
		bool checkonly = !!(flags & CTF_CheckOnly);

		int exx = !!(flags & CTF_Expand);

		W = GetWidth() + 2 * exx;
		H = GetHeight() + 2 * exx;

		if (!checkonly)
		{
			auto remap = translation <= 0 || IsLuminosityTranslation(translation) ? nullptr : GPalette.TranslationToTable(translation);
			if (remap && remap->Inactive) remap = nullptr;
			if (remap) translation = remap->Index;

			int trans;
			auto Pixels = GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
			
			if(!exx && Pixels.ClipRect.x == 0 && Pixels.ClipRect.y == 0 && Pixels.ClipRect.width == Pixels.Width && Pixels.ClipRect.height == Pixels.Height && (Pixels.FreeBuffer || !IsLuminosityTranslation(translation)))
			{
				buffer = Pixels.data;
				result.mFreeBuffer = Pixels.FreeBuffer;
				Pixels.FreeBuffer = false;
			}
			else
			{
				buffer = new unsigned char[W * (H + 1) * 4];
				memset(buffer, 0, W * (H + 1) * 4);

				FBitmap bmp(buffer, W * 4, W, H);

				bmp.Blit(exx, exx, Pixels);
			}
			
			if (IsLuminosityTranslation(translation))
			{
				V_ApplyLuminosityTranslation(LuminosityTranslationDesc::fromInt(translation), buffer, W * H);
			}

			if (remap == nullptr)
			{
				CheckTrans(buffer, W * H, trans);
				isTransparent = bTranslucent;
			}
			else
			{
				isTransparent = 0;
				// A translated image is not conclusive for setting the texture's transparency info.
			}
		}

		if (GetImage())
		{
			FContentIdBuilder builder;
			builder.id = 0;
			builder.imageID = GetImage()->GetId();
			builder.translation = max(0, translation);
			builder.expand = exx;
			result.mContentId = builder.id;
		}
		else result.mContentId = 0;	// for non-image backed textures this has no meaning so leave it at 0.

		result.mBuffer = buffer;
		result.mWidth = W;
		result.mHeight = H;

		// Only do postprocessing for image-backed textures. (i.e. not for the burn texture which can also pass through here.)
		if (GetImage() && flags & CTF_ProcessData)
		{
			if (flags & CTF_Upscale) CreateUpsampledTextureBuffer(result, !!isTransparent, checkonly);

			if (!checkonly) ProcessData(result.mBuffer, result.mWidth, result.mHeight, false);
		}
	}
	return result;

}

bool FTexture::CreatePixelSnapshot(int translation, int flags,
	FTexturePixelSnapshot &snapshot)
{
	if (flags & CTF_CheckOnly)
		return false;

	snapshot = {};
	snapshot.width = GetWidth();
	snapshot.height = GetHeight();
	snapshot.pixelWidth = snapshot.width;
	snapshot.pixelHeight = snapshot.height;
	if (snapshot.width <= 0 || snapshot.height <= 0)
		return false;
	snapshot.flags = flags;
	snapshot.translation = translation;
	snapshot.indexed = (flags & CTF_Indexed) != 0;
	snapshot.sourceMasked = Masked;
	snapshot.sourceTranslucency = bTranslucent;
	snapshot.sourceAreaCount = areacount;
	snapshot.hasImage = GetImage() != nullptr;
	if (snapshot.hasImage)
		snapshot.imageId = GetImage()->GetId();
	snapshot.upscaleMode = gl_texture_hqresizemode;
	snapshot.upscaleMultiplier = gl_texture_hqresizemult;

	if (snapshot.indexed)
	{
		auto store = Get8BitPixels(false);
		if (store.Size() < (size_t)snapshot.width * snapshot.height)
			return false;
		snapshot.pitch = snapshot.width;
		snapshot.clipWidth = snapshot.width;
		snapshot.clipHeight = snapshot.height;
		snapshot.pixels.assign(store.Data(), store.Data() +
			(size_t)snapshot.width * snapshot.height);
		return true;
	}

	auto remap = translation <= 0 || IsLuminosityTranslation(translation)
		? nullptr : GPalette.TranslationToTable(translation);
	if (remap && remap->Inactive)
		remap = nullptr;
	snapshot.remapped = remap != nullptr;
	if (remap)
		snapshot.translation = remap->Index;
	int trans = -1;
	FBitmap pixels = GetBgraBitmap(remap ? remap->Palette : nullptr, &trans);
	if (!pixels.GetPixels() || pixels.GetWidth() <= 0 || pixels.GetHeight() <= 0 ||
		pixels.GetPitch() < pixels.GetWidth() * 4)
		return false;
	snapshot.pixelWidth = pixels.GetWidth();
	snapshot.pixelHeight = pixels.GetHeight();
	snapshot.pitch = pixels.GetPitch();
	snapshot.clipX = pixels.GetClipRect().x;
	snapshot.clipY = pixels.GetClipRect().y;
	snapshot.clipWidth = pixels.GetClipRect().width;
	snapshot.clipHeight = pixels.GetClipRect().height;
	snapshot.sourceTransparency = trans;
	snapshot.pitch = snapshot.pixelWidth * 4;
	snapshot.pixels.resize((size_t)snapshot.pitch * snapshot.pixelHeight);
	for (int y = 0; y < snapshot.pixelHeight; y++)
		memcpy(snapshot.pixels.data() + (size_t)y * snapshot.pitch,
			pixels.GetPixels() + (size_t)y * pixels.GetPitch(), snapshot.pitch);
	return true;
}

static int FindSnapshotHoles(const uint8_t *buffer, int width, int height,
	int sourceAreaCount, bool hasImage, std::vector<FloatRect> &areas)
{
	if (sourceAreaCount != 0)
		return sourceAreaCount;
	int areaCount = -1;
	if (height > 512 || !hasImage)
		return areaCount;

	int gaps[5][2];
	int gapc = 0;
	int startdraw = -1;
	int lendraw = 0;
	for (int y = 0; y < height; y++)
	{
		int x = 0;
		const unsigned char *line = buffer + width * y * 4 + 3;
		for (; x < width; x++, line += 4)
			if (*line != 0) break;

		if (x != width)
		{
			if (startdraw == -1)
			{
				startdraw = y;
				if (gapc && y <= gaps[gapc - 1][0] + gaps[gapc - 1][1] + 16)
				{
					gapc--;
					startdraw = gaps[gapc][0];
					lendraw = y - startdraw;
				}
				if (gapc == 4)
					return areaCount;
			}
			lendraw++;
		}
		else if (startdraw != -1)
		{
			if (lendraw == 1) lendraw = 2;
			gaps[gapc][0] = startdraw;
			gaps[gapc][1] = lendraw;
			gapc++;
			startdraw = -1;
			lendraw = 0;
		}
	}
	if (startdraw != -1)
	{
		gaps[gapc][0] = startdraw;
		gaps[gapc][1] = lendraw;
		gapc++;
	}
	if (startdraw == 0 && lendraw == height)
		return areaCount;
	for (int i = 0; i < gapc; i++)
		areas.push_back({ -1.0f, (float)gaps[i][0] / height, -1.0f,
			(float)gaps[i][1] / height });
	return gapc;
}

bool FTexture::ProcessPixelSnapshot(const FTexturePixelSnapshot &snapshot,
	FTexturePixelResult &result)
{
	result = {};
	result.sourceAreaCount = snapshot.sourceAreaCount;
	result.sourceTranslucency = snapshot.sourceTranslucency;
	result.translucency = snapshot.sourceTranslucency;
	result.sourceMasked = snapshot.sourceMasked;
	result.masked = snapshot.sourceMasked;
	result.indexed = snapshot.indexed;
	result.width = snapshot.width;
	result.height = snapshot.height;
	result.bytesPerPixel = snapshot.indexed ? 1 : 4;

	if (snapshot.indexed)
	{
		if (snapshot.pixels.size() < (size_t)snapshot.width * snapshot.height)
			return false;
		result.pixels.resize((size_t)snapshot.width * snapshot.height);
		ImageHelpers::FlipNonSquareBlock(result.pixels.data(), snapshot.pixels.data(),
			snapshot.height, snapshot.width, snapshot.height);
		return true;
	}

	const bool expand = (snapshot.flags & CTF_Expand) != 0;
	const int width = snapshot.width + (expand ? 2 : 0);
	const int height = snapshot.height + (expand ? 2 : 0);
	if (width <= 0 || height <= 0 || snapshot.pitch <= 0 ||
		snapshot.pixels.size() < (size_t)snapshot.pitch * snapshot.pixelHeight)
		return false;
	const bool fullClip = snapshot.clipX == 0 && snapshot.clipY == 0 &&
		snapshot.clipWidth == snapshot.pixelWidth && snapshot.clipHeight == snapshot.pixelHeight &&
		snapshot.pixelWidth == width && snapshot.pixelHeight == height;
	uint8_t *buffer = nullptr;
	if (!expand && fullClip)
	{
		result.pixels = snapshot.pixels;
		buffer = result.pixels.data();
	}
	else
	{
		result.pixels.assign((size_t)width * (height + 1) * 4, 0);
		FBitmap src(const_cast<uint8_t *>(snapshot.pixels.data()), snapshot.pitch,
			snapshot.pixelWidth, snapshot.pixelHeight);
		FClipRect clip{ snapshot.clipX, snapshot.clipY, snapshot.clipWidth, snapshot.clipHeight };
		src.SetClipRect(clip);
		FBitmap dst(result.pixels.data(), width * 4, width, height);
		dst.Blit(expand, expand, src);
		buffer = result.pixels.data();
	}

	if (IsLuminosityTranslation(snapshot.translation))
		V_ApplyLuminosityTranslation(LuminosityTranslationDesc::fromInt(snapshot.translation),
			buffer, width * height);

	int isTransparent = snapshot.remapped ? 0 : snapshot.sourceTransparency;
	result.width = width;
	result.height = height;
	if (!snapshot.remapped && result.sourceTranslucency == -1)
	{
		if (isTransparent == -1)
		{
			result.translucency = 0;
			const uint32_t *pixels = (const uint32_t *)buffer;
			for (int i = 0; i < width * height; i++)
			{
				uint32_t alpha = pixels[i] >> 24;
				if (alpha != 0xff && alpha != 0)
				{
					result.translucency = 1;
					break;
				}
			}
		}
		else
			result.translucency = isTransparent;
		result.hasTranslucency = true;
	}

	if (snapshot.hasImage && (snapshot.flags & CTF_ProcessData))
	{
		if (snapshot.flags & CTF_Upscale)
		{
			FTextureBuffer texbuffer;
			size_t size = (size_t)width * height * 4;
			texbuffer.mBuffer = new uint8_t[size];
			memcpy(texbuffer.mBuffer, buffer, size);
			texbuffer.mWidth = width;
			texbuffer.mHeight = height;
			FContentIdBuilder builder;
			builder.id = 0;
			builder.imageID = snapshot.imageId;
			builder.translation = std::max(0, snapshot.translation);
			builder.expand = expand;
			texbuffer.mContentId = builder.id;
			CreateUpsampledTextureBufferForSnapshot(texbuffer,
				!snapshot.remapped && result.translucency != 0, false, snapshot.upscaleMode,
				snapshot.upscaleMultiplier);
			if (!texbuffer.mBuffer)
				return false;
			result.width = texbuffer.mWidth;
			result.height = texbuffer.mHeight;
			result.pixels.assign(texbuffer.mBuffer,
				texbuffer.mBuffer + (size_t)result.width * result.height * 4);
		}
		result.hasMasking = snapshot.sourceMasked;
		if (snapshot.sourceMasked)
		{
			result.masked = SmoothEdges(result.pixels.data(), result.width, result.height);
			if (result.masked && snapshot.sourceAreaCount == 0)
			{
				result.hasAreas = true;
				result.areaCount = FindSnapshotHoles(result.pixels.data(), result.width,
					result.height, snapshot.sourceAreaCount, snapshot.hasImage, result.areas);
			}
		}
	}
	return true;
}

void FTexture::ApplyPixelSnapshotResult(const FTexturePixelResult &result)
{
	if (result.hasTranslucency && bTranslucent == result.sourceTranslucency)
		bTranslucent = result.translucency;
	if (result.hasMasking && Masked == result.sourceMasked)
		Masked = result.masked;
	if (!result.hasAreas || areacount != result.sourceAreaCount)
		return;
	areacount = result.areaCount;
	if (result.areaCount <= 0)
	{
		areas = nullptr;
		return;
	}
	areas = (FloatRect *)ImageArena.Alloc(result.areas.size() * sizeof(FloatRect));
	memcpy(areas, result.areas.data(), result.areas.size() * sizeof(FloatRect));
}

static bool PixelSnapshotBuffersMatch(const FTextureBuffer &reference,
	const FTexturePixelResult &snapshot, size_t byteCount, size_t *firstMismatch = nullptr)
{
	if (!reference.mBuffer || reference.mWidth != snapshot.width ||
		reference.mHeight != snapshot.height || snapshot.pixels.size() < byteCount)
		return false;
	for (size_t i = 0; i < byteCount; ++i)
	{
		if (reference.mBuffer[i] != snapshot.pixels[i])
		{
			if (firstMismatch) *firstMismatch = i;
			return false;
		}
	}
	return true;
}

class FTexturePixelSnapshotSelfTestState
{
	FTexture &texture;
	bool masked;
	int8_t translucent;
	int8_t areaCount;
	FloatRect *areas;

public:
	explicit FTexturePixelSnapshotSelfTestState(FTexture &texture)
		: texture(texture), masked(texture.Masked), translucent(texture.bTranslucent),
		  areaCount(texture.areacount), areas(texture.areas)
	{
	}

	~FTexturePixelSnapshotSelfTestState()
	{
		texture.Masked = masked;
		texture.bTranslucent = translucent;
		texture.areacount = areaCount;
		texture.areas = areas;
	}
};

// Compare the detached worker result with the established conversion path
// using the same live FTexture and flags. Keep this explicit-name diagnostic
// bounded: callers choose the fixtures instead of forcing a full texture scan.
CCMD(r_texture_snapshot_selftest)
{
	if (argv.argc() < 2)
	{
		Printf("Usage: r_texture_snapshot_selftest <texture> [texture ...]\n");
		return;
	}

	static const int testFlags[] = {
		CTF_ProcessData,
		CTF_ProcessData | CTF_Expand,
		CTF_ProcessData | CTF_Upscale,
		CTF_ProcessData | CTF_Expand | CTF_Upscale,
		CTF_ProcessData | CTF_Indexed,
	};
	const int iceTranslation = TRANSLATION(TRANSLATION_Standard, STD_Ice).index();
	FRemapTable *iceRemap = GPalette.TranslationToTable(iceTranslation);
	if (!iceRemap || iceRemap->Inactive || iceRemap->IsIdentity())
	{
		Printf("snapshot self-test: non-identity Ice translation unavailable\n");
		return;
	}
	int comparisons = 0;
	int failures = 0;
	bool negativeControlPassed = false;

	for (int arg = 1; arg < argv.argc(); ++arg)
	{
		FTextureID id = TexMan.CheckForTexture(argv[arg], ETextureType::Any,
			TexMan.TEXMAN_TryAny);
		FGameTexture *gameTexture = TexMan.GetGameTexture(id);
		FTexture *texture = gameTexture ? gameTexture->GetTexture() : nullptr;
		if (!id.Exists() || !texture || texture->isHardwareCanvas())
		{
			Printf("snapshot self-test: %s: texture not found or unsupported\n", argv[arg]);
			++failures;
			continue;
		}

		// CreateTexBuffer() runs ProcessData(), which mutates mask/translucency
		// and hole-area metadata. Keep this diagnostic from changing the live
		// texture's state while comparing conversion outputs.
		FTexturePixelSnapshotSelfTestState restoreState(*texture);
		for (int flags : testFlags)
		{
			// CTF_Indexed explicitly ignores remaps. Exercise both identity and
			// non-identity source-pixel paths for every RGBA conversion mode.
			const int translations[] = { 0, iceTranslation };
			const int translationCount = (flags & CTF_Indexed) ? 1 : 2;
			for (int translationIndex = 0; translationIndex < translationCount;
				++translationIndex)
			{
				const int translation = translations[translationIndex];
				FTexturePixelSnapshot source;
				FTexturePixelResult processed;
				if (!texture->CreatePixelSnapshot(translation, flags, source) ||
					!FTexture::ProcessPixelSnapshot(source, processed))
				{
					Printf("snapshot self-test: %s translation=%d flags=0x%x: "
						"snapshot processing failed\n", argv[arg], translation, flags);
					++failures;
					continue;
				}

				FTextureBuffer reference = texture->CreateTexBuffer(translation, flags);
				const size_t bytesPerPixel = (flags & CTF_Indexed) ? 1 : 4;
				const size_t byteCount = (size_t)reference.mWidth * reference.mHeight * bytesPerPixel;
				size_t mismatch = 0;
				if (processed.indexed != !!(flags & CTF_Indexed) ||
					processed.bytesPerPixel != (int)bytesPerPixel ||
					!PixelSnapshotBuffersMatch(reference, processed, byteCount,
					&mismatch))
				{
					Printf("snapshot self-test: %s translation=%d flags=0x%x: "
						"mismatch at byte %zu (sync=%dx%d snapshot=%dx%d)\n",
						argv[arg], translation, flags, mismatch, reference.mWidth,
						reference.mHeight, processed.width, processed.height);
					++failures;
					continue;
				}
				++comparisons;

				if (!negativeControlPassed && byteCount > 0)
				{
					FTexturePixelResult corrupted = processed;
					corrupted.pixels[0] ^= 1;
					negativeControlPassed = !PixelSnapshotBuffersMatch(reference,
						corrupted, byteCount);
				}
			}
		}
	}

	bool passed = comparisons > 0 && failures == 0 && negativeControlPassed;
	Printf("snapshot self-test: %s (%d exact comparisons incl. Ice remap, "
		"%d failures, one-byte negative control %s)\n", passed ? "PASS" : "FAIL",
		comparisons, failures, negativeControlPassed ? "detected" : "missed");
}

//===========================================================================
//
// Dummy texture for the 0-entry.
//
//===========================================================================

bool FTexture::DetermineTranslucency()
{
		// This will calculate all we need, so just discard the result.
		CreateTexBuffer(0);
	return !!bTranslucent;
}

//===========================================================================
// 
// the default just returns an empty texture.
//
//===========================================================================

TArray<uint8_t> FTexture::Get8BitPixels(bool alphatex)
{
	TArray<uint8_t> Pixels(Width * Height, true);
	memset(Pixels.Data(), 0, Width * Height);
	return Pixels;
}

//===========================================================================
// 
//  Finds empty space around the texture. 
//  Used for sprites that got placed into a huge empty frame.
//
//===========================================================================

bool FTexture::TrimBorders(uint16_t* rect)
{

	auto texbuffer = CreateTexBuffer(0);
	int w = texbuffer.mWidth;
	int h = texbuffer.mHeight;
	auto Buffer = texbuffer.mBuffer;

	if (texbuffer.mBuffer == nullptr)
	{
		return false;
	}
	if (w != Width || h != Height)
	{
		// external Hires replacements cannot be trimmed.
		return false;
	}

	int size = w * h;
	if (size == 1)
	{
		// nothing to be done here.
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}
	int first, last;

	for (first = 0; first < size; first++)
	{
		if (Buffer[first * 4 + 3] != 0) break;
	}
	if (first >= size)
	{
		// completely empty
		rect[0] = 0;
		rect[1] = 0;
		rect[2] = 1;
		rect[3] = 1;
		return true;
	}

	for (last = size - 1; last >= first; last--)
	{
		if (Buffer[last * 4 + 3] != 0) break;
	}

	rect[1] = first / w;
	rect[3] = 1 + last / w - rect[1];

	rect[0] = 0;
	rect[2] = w;

	unsigned char* bufferoff = Buffer + (rect[1] * w * 4);
	h = rect[3];

	for (int x = 0; x < w; x++)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0) goto outl;
		}
		rect[0]++;
	}
outl:
	rect[2] -= rect[0];

	for (int x = w - 1; rect[2] > 1; x--)
	{
		for (int y = 0; y < h; y++)
		{
			if (bufferoff[(x + y * w) * 4 + 3] != 0)
			{
				return true;
			}
		}
		rect[2]--;
	}
	return true;
}

//===========================================================================
//
// Create a hardware texture for this texture image.
//
//===========================================================================

IHardwareTexture* FTexture::GetHardwareTexture(int translation, int scaleflags)
{
	int indexed = scaleflags & CTF_Indexed;
	if (indexed) translation = -1;
	IHardwareTexture* hwtex = SystemTextures.GetHardwareTexture(translation, scaleflags);
	if (hwtex == nullptr)
	{
		hwtex = screen->CreateHardwareTexture(indexed? 1 : 4);
		SystemTextures.AddHardwareTexture(translation, scaleflags, hwtex);
	}
	return hwtex;
}


//==========================================================================
//
// this must be copied back to textures.cpp later.
//
//==========================================================================

FWrapperTexture::FWrapperTexture(int w, int h, int bits)
{
	Width = w;
	Height = h;
	Format = bits;
	//bNoCompress = true;
	auto hwtex = screen->CreateHardwareTexture(4);
	// todo: Initialize here.
	SystemTextures.AddHardwareTexture(0, false, hwtex);
}
