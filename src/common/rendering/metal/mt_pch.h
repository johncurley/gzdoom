#pragma once

#include "metal/system/mt_renderdevice.h"
#include "metal/system/mt_commandbuffer.h"
#include "metal/system/mt_buffer.h"
#include "metal/system/mt_hwbuffer.h"
#include "metal/renderer/mt_renderstate.h"
#include "metal/renderer/mt_pipelinestate.h"
#include "metal/renderer/mt_streambuffer.h"
#include "metal/renderer/mt_renderbuffers.h"
#include "metal/renderer/mt_postprocess.h"
#include "metal/renderer/mt_resourcebinding.h"
#include "metal/shaders/mt_shader.h"
#include "metal/textures/mt_texture.h"
#include "metal/textures/mt_sampler.h"

#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"

#include <Metal/Metal.hpp>
