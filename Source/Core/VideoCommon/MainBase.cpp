// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Event.h"
#include "Core/ConfigManager.h"


#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/MainBase.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoState.h"

bool s_BackendInitialized = false;

static Common::Flag s_FifoShuttingDown;
volatile u32 s_EFB_PCache_Frame;

static volatile struct
{
	u32 xfbAddr;
	u32 fbWidth;
	u32 fbHeight;
	u32 fbStride;
} s_beginFieldArgs;

void VideoBackendHardware::EmuStateChange(EMUSTATE_CHANGE newState)
{
	EmulatorState((newState == EMUSTATE_CHANGE_PLAY) ? true : false);
}

// Enter and exit the video loop
void VideoBackendHardware::Video_EnterLoop()
{
	RunGpuLoop();
}

void VideoBackendHardware::Video_ExitLoop()
{
	ExitGpuLoop();
	s_FifoShuttingDown.Set();
}

void VideoBackendHardware::Video_SetRendering(bool bEnabled)
{
	Fifo_SetRendering(bEnabled);
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoBackendHardware::Video_BeginField(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight)
{
	if (s_BackendInitialized && g_ActiveConfig.bUseXFB)
	{
		s_beginFieldArgs.xfbAddr = xfbAddr;
		s_beginFieldArgs.fbWidth = fbWidth;
		s_beginFieldArgs.fbStride = fbStride;
		s_beginFieldArgs.fbHeight = fbHeight;
	}
}

// Run from the CPU thread (from VideoInterface.cpp)
void VideoBackendHardware::Video_EndField()
{
	if (s_BackendInitialized && g_ActiveConfig.bUseXFB && g_renderer)
	{
		AsyncRequests::Event e;
		e.time = 0;
		e.type = AsyncRequests::Event::SWAP_EVENT;
		e.swap_event.xfbAddr = s_beginFieldArgs.xfbAddr;
		e.swap_event.fbWidth = s_beginFieldArgs.fbWidth;
		e.swap_event.fbStride = s_beginFieldArgs.fbStride;
		e.swap_event.fbHeight = s_beginFieldArgs.fbHeight;
		AsyncRequests::GetInstance()->PushEvent(e, false);
	}
}

void VideoBackendHardware::Video_AddMessage(const std::string& str, u32 milliseconds)
{
	OSD::AddMessage(str, milliseconds);
}

void VideoBackendHardware::Video_ClearMessages()
{
	OSD::ClearMessages();
}

// Screenshot
bool VideoBackendHardware::Video_Screenshot(const std::string& _szFilename)
{
	Renderer::SetScreenshot(_szFilename.c_str());
	return true;
}

VideoBackendHardware::VideoBackendHardware()
{
	// TODO: Make this values configurable
	// Scale aplied to reduce peek cache size
	m_EFB_PCache_Divisor = 3;
	// Lifespam of the cache values in frames
	m_EFB_PCache_Life = 3;
	m_EFB_PCache_Width = EFB_WIDTH >> m_EFB_PCache_Divisor;
	m_EFB_PCache_Height = EFB_HEIGHT >> m_EFB_PCache_Divisor;
	m_EFB_PCache_Size = m_EFB_PCache_Width * m_EFB_PCache_Height;
	m_EFB_PCache = new EFBPeekCacheElement[m_EFB_PCache_Size];	
}

VideoBackendHardware::~VideoBackendHardware()
{
	if (m_EFB_PCache)
	{
		delete [] m_EFB_PCache;
	}
}

u32 VideoBackendHardware::Video_AccessEFB(EFBAccessType type, u32 x, u32 y, u32 InputData)
{
	if (!(g_ActiveConfig.bEFBAccessEnable && s_BackendInitialized))
	{
		return 0;
	}
	u32 result = InputData;
	u32 efb_p_cache_stride = (y >> m_EFB_PCache_Divisor) * m_EFB_PCache_Width + (x >> m_EFB_PCache_Divisor);
	if (type == POKE_COLOR || type == POKE_Z)
	{
		AsyncRequests::Event e;
		e.type = type == POKE_COLOR ? AsyncRequests::Event::EFB_POKE_COLOR : AsyncRequests::Event::EFB_POKE_Z;
		e.time = 0;
		e.efb_poke.data = InputData;
		e.efb_poke.x = x;
		e.efb_poke.y = y;
		AsyncRequests::GetInstance()->PushEvent(e, false);
	}
	else
	{			
		if (g_ActiveConfig.bEFBFastAccess)
		{
			if (type == PEEK_COLOR && m_EFB_PCache[efb_p_cache_stride].ColorFrame > s_EFB_PCache_Frame)
			{
				return m_EFB_PCache[efb_p_cache_stride].ColorValue;
			}
			else if (type == PEEK_Z && m_EFB_PCache[efb_p_cache_stride].DepthFrame > s_EFB_PCache_Frame)
			{
				return m_EFB_PCache[efb_p_cache_stride].DepthValue;
			}
		}
		AsyncRequests::Event e;

		e.type = type == PEEK_COLOR ? AsyncRequests::Event::EFB_PEEK_COLOR : AsyncRequests::Event::EFB_PEEK_Z;
		e.time = 0;
		e.efb_peek.x = x;
		e.efb_peek.y = y;
		e.efb_peek.data = &result;
		AsyncRequests::GetInstance()->PushEvent(e, true);
	}
	if (g_ActiveConfig.bEFBFastAccess)
	{
		if (type == PEEK_COLOR || type == POKE_COLOR)
		{
			m_EFB_PCache[efb_p_cache_stride].ColorValue = result;
			m_EFB_PCache[efb_p_cache_stride].ColorFrame = s_EFB_PCache_Frame + m_EFB_PCache_Life;
		}
		else
		{
			m_EFB_PCache[efb_p_cache_stride].DepthValue = result;
			m_EFB_PCache[efb_p_cache_stride].DepthFrame = s_EFB_PCache_Frame + m_EFB_PCache_Life;
		}
	}
	return result;
}

u32 VideoBackendHardware::Video_GetQueryResult(PerfQueryType type)
{
	if (!g_perf_query->ShouldEmulate())
	{
		return 0;
	}
	AsyncRequests::Event e;
	e.time = 0;
	e.type = AsyncRequests::Event::PERF_QUERY;
	if (!g_perf_query->IsFlushed())
		AsyncRequests::GetInstance()->PushEvent(e, true);
	return g_perf_query->GetQueryResult(type);
}
u16 VideoBackendHardware::Video_GetBoundingBox(int index)
{
	if (!g_ActiveConfig.backend_info.bSupportsBBox || g_ActiveConfig.iBBoxMode != BBoxGPU)
		return BoundingBox::coords[index];
	AsyncRequests::Event e;
	u16 result;
	e.time = 0;
	e.type = AsyncRequests::Event::BBOX_READ;
	e.bbox.index = index;
	e.bbox.data = &result;
	AsyncRequests::GetInstance()->PushEvent(e, true);
	return result;
}

void VideoBackendHardware::InitializeShared()
{
	VideoCommon_Init();

	s_FifoShuttingDown.Clear();
	memset((void*)&s_beginFieldArgs, 0, sizeof(s_beginFieldArgs));
	m_invalid = false;
	memset(m_EFB_PCache, 0 , m_EFB_PCache_Size * sizeof(EFBPeekCacheElement));
	s_EFB_PCache_Frame = 1;
}

// Run from the CPU thread
void VideoBackendHardware::DoState(PointerWrap& p)
{
	bool software = false;
	p.Do(software);

	if (p.GetMode() == PointerWrap::MODE_READ && software == true)
	{
		// change mode to abort load of incompatible save state.
		p.SetMode(PointerWrap::MODE_VERIFY);
	}

	VideoCommon_DoState(p);
	p.DoMarker("VideoCommon");

	p.Do(s_beginFieldArgs);
	p.DoMarker("VideoBackendHardware");

	// Refresh state.
	if (p.GetMode() == PointerWrap::MODE_READ)
	{
		m_invalid = true;
		// Clear all caches that touch RAM
		// (? these don't appear to touch any emulation state that gets saved. moved to on load only.)
		MarkAllAttrDirty();
	}
}

void VideoBackendHardware::CheckInvalidState()
{
	if (m_invalid)
	{
		m_invalid = false;
		
		BPReload();
		TextureCache::Invalidate();
	}
}

void VideoBackendHardware::PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
	Fifo_PauseAndLock(doLock, unpauseOnUnlock);
}


void VideoBackendHardware::RunLoop(bool enable)
{
	VideoCommon_RunLoop(enable);
}

void VideoBackendHardware::Video_GatherPipeBursted()
{
	CommandProcessor::GatherPipeBursted();
}

bool VideoBackendHardware::Video_IsPossibleWaitingSetDrawDone()
{
	return CommandProcessor::isPossibleWaitingSetDrawDone;
}

void VideoBackendHardware::RegisterCPMMIO(MMIO::Mapping* mmio, u32 base)
{
	CommandProcessor::RegisterMMIO(mmio, base);
}

void VideoBackendHardware::UpdateWantDeterminism(bool want)
{
	//TODO: Implement
	//Fifo_UpdateWantDeterminism(want);
}
