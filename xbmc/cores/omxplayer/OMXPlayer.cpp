/*
 *      Copyright (C) 2011 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h"

#if defined (HAVE_OMXPLAYER)
#include "OMXPlayer.h"
#include "Application.h"
#include "FileItem.h"
#include "GUIInfoManager.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIWindowManager.h"
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "windowing/WindowingFactory.h"
#include "utils/log.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/URIUtils.h"
#include "utils/XMLUtils.h"

#include "FileItem.h"
#include "filesystem/File.h"
#include "utils/BitstreamStats.h"

#include "utils/LangCodeExpander.h"
#include "utils/StreamDetails.h"

#include <sstream>
#include <iomanip>

#include "BitstreamConverter.h"

using namespace XFILE;

// ****************************************************************
// ****************************************************************
COMXPlayer::COMXPlayer(IPlayerCallback &callback) 
  : IPlayer(callback),
  CThread("COMXPlayer"),
  m_ready(true)
{
  m_speed = 1;
  m_paused = false;
  m_StopPlaying = false;
  m_mode3d_sbs = false;

  m_OMX.Initialize();

  m_av_clock = new OMXClock();
}

COMXPlayer::~COMXPlayer()
{
  CloseFile();

  if(m_av_clock)
    delete m_av_clock;
  m_av_clock = NULL;

  m_OMX.Deinitialize();
}

bool COMXPlayer::Initialize(TiXmlElement* pConfig)
{
  return true;
}

bool COMXPlayer::OpenVideoDecoder(COMXStreamInfo hints)
{
  m_video_codec_name = "";

  RESOLUTION res = g_graphicsContext.GetVideoResolution();
  m_video_width   = g_settings.m_ResInfo[res].iWidth;
  m_video_height  = g_settings.m_ResInfo[res].iHeight;

  if (m_hints_video.fpsrate && m_hints_video.fpsscale)
    m_video_fps = DVD_TIME_BASE / OMXReader::NormalizeFrameduration((double)DVD_TIME_BASE * m_hints_video.fpsscale / m_hints_video.fpsrate);
  else
    m_video_fps = 25;

  if( m_video_fps > 100 || m_video_fps < 5 )
  {
    printf("Invalid framerate %d, using forced 25fps and just trust timestamps\n", (int)m_video_fps);
    m_video_fps = 25;
  }

  m_frametime = (double)DVD_TIME_BASE / m_video_fps;

  m_video_decoder = new COMXVideo();
  if(!m_video_decoder->Open(m_hints_video, m_av_clock, true))
  {
    delete m_video_decoder;
    m_video_decoder = NULL;
    return false;
  }
  else
  {
    m_video_codec_name = m_video_decoder->GetDecoderName();
    printf("Video codec %s width %d height %d profile %d r_frame_rate.num %d r_frame_rate.den %d\n",
        m_video_codec_name.c_str(), m_hints_video.width, m_hints_video.height, m_hints_video.profile, 
        m_hints_video.fpsscale, m_hints_video.fpsrate);
  }

  return true;
}

void COMXPlayer::CloseVideoDecoder()
{
  if(m_video_decoder)
    delete m_video_decoder;
  m_video_decoder   = NULL;
  m_video_codec_name = "";
}

bool COMXPlayer::OpenAudioCodec(COMXStreamInfo hints)
{
  m_pAudioCodec = new COMXAudioCodecOMX();

  if(!m_pAudioCodec->Open(m_hints_audio))
  {
    delete m_pAudioCodec; 
    m_pAudioCodec = NULL;
    return false;
  }

  return true;
}

void COMXPlayer::CloseAudioCodec()
{
  if(m_pAudioCodec)
    delete m_pAudioCodec;
  m_pAudioCodec = NULL;
}

IAudioRenderer::EEncoded COMXPlayer::IsPassthrough(COMXStreamInfo hints)
{
  int  m_outputmode = 0;
  bool bitstream = false;
  IAudioRenderer::EEncoded passthrough = IAudioRenderer::ENCODED_NONE;

  m_outputmode = g_guiSettings.GetInt("audiooutput.mode");

  switch(m_outputmode)
  {
    case 0:
      passthrough = IAudioRenderer::ENCODED_NONE;
      break;
    case 1:
      bitstream = true;
      break;
    case 2:
      bitstream = true;
      break;
  }

  if(bitstream)
  {
    if(hints.codec == CODEC_ID_AC3 && g_guiSettings.GetBool("audiooutput.ac3passthrough"))
    {
      passthrough = IAudioRenderer::ENCODED_IEC61937_AC3;
    }
    if(hints.codec == CODEC_ID_DTS && g_guiSettings.GetBool("audiooutput.dtspassthrough"))
    {
      passthrough = IAudioRenderer::ENCODED_IEC61937_DTS;
    }
  }

  return passthrough;
}

bool COMXPlayer::OpenAudioDecoder(COMXStreamInfo hints)
{
  bool bAudioRenderOpen = false;

  if(!m_pAudioCodec)
    return false;

  m_pChannelMap = m_pAudioCodec->GetChannelMap();

  m_audio_render = new COMXAudio();
  m_audio_render->SetClock(m_av_clock);

  CStdString deviceString;

  if(m_Passthrough)
  {
    m_HWDecode = false;

    deviceString = g_guiSettings.GetString("audiooutput.passthroughdevice");
    //m_audio_render->SetCodingType(m_hints_audio.codec);

    //m_hints_audio.channels = 2;
    bAudioRenderOpen = m_audio_render->Initialize(NULL, deviceString.substr(4), m_pChannelMap,
                                                   hints, m_av_clock, m_Passthrough, m_HWDecode);
  }
  else
  {
    deviceString = g_guiSettings.GetString("audiooutput.audiodevice");
    //m_audio_render->SetCodingType(CODEC_ID_PCM_S16LE);

    if(m_HWDecode)
    {
      bAudioRenderOpen = m_audio_render->Initialize(NULL, deviceString.substr(4), m_pChannelMap,
                                                     hints, m_av_clock, m_Passthrough, m_HWDecode);
    }
    else
    {
      bAudioRenderOpen = m_audio_render->Initialize(NULL, deviceString.substr(4), m_pAudioCodec->GetChannels(), m_pChannelMap,
          m_pAudioCodec->GetSampleRate(), m_pAudioCodec->GetBitsPerSample(), false, false, m_Passthrough);
    }
  }

  if(!bAudioRenderOpen)
  {
    delete m_audio_render;
    m_audio_render = NULL;
    return false;
  }
  else
  {
    if(m_Passthrough)
    {
      printf("Audio codec %s channels %d samplerate %d bitspersample %d\n",
        m_audio_codec_name.c_str(), 2, hints.samplerate, hints.bitspersample);
    }
    else
    {
      printf("Audio codec %s channels %d samplerate %d bitspersample %d\n",
        m_audio_codec_name.c_str(), hints.channels, hints.samplerate, hints.bitspersample);
    }
  }

  m_audio_codec_name = m_omx_reader.GetAudioCodecName();
  return true;
}

void COMXPlayer::CloseAudioDecoder()
{
  if(m_audio_render)
    delete m_audio_render;
  m_audio_render  = NULL;
  m_audio_codec_name = "";
}

void COMXPlayer::ResetStreams()
{
  if(m_av_clock)
    m_av_clock->Pause();

  if(m_pAudioCodec)
    m_pAudioCodec->Reset();

  if(m_video_decoder)
    m_video_decoder->Reset();

  if(m_audio_render)
    m_audio_render->Flush();

  if(m_video_pkt)
  {
    m_omx_reader.FreePacket(m_video_pkt);
    m_video_pkt = NULL;
  }

  if(m_audio_pkt)
  {
    m_omx_reader.FreePacket(m_audio_pkt);
    m_audio_pkt = NULL;
  }

  if(m_av_clock)
    m_av_clock->Reset();
}

bool COMXPlayer::OpenFile(const CFileItem &file, const CPlayerOptions &options)
{
  try
  {
    CLog::Log(LOGNOTICE, "COMXPlayer: Opening: %s", file.GetPath().c_str());
    // if playing a file close it first
    // this has to be changed so we won't have to close it.
    if(ThreadHandle())
      CloseFile();

    unsigned char *buffer = NULL;
    int           result  = -1;
    AVInputFormat       *iformat          = NULL;

    std::string url;

    m_item = file;
    m_options = options;
    m_StopPlaying = false;

    m_elapsed_ms  = 0;
    m_duration_ms = 0;

    m_audio_index = 0;
    m_audio_count = 0;

    m_audio_change = true;

    m_video_index = 0;
    m_video_count = 0;
    m_video_fps   = 0.0;
    m_video_width = 0;
    m_video_height= 0;

    m_subtitle_index = 0;
    m_subtitle_count = 0;
    m_chapter_count  = 0;

    m_subtitle_show  = g_settings.m_currentVideoSettings.m_SubtitleOn;

    SetAVDelay(g_settings.m_currentVideoSettings.m_AudioDelay);
    SetSubTitleDelay(g_settings.m_currentVideoSettings.m_SubtitleDelay);

    m_hints_audio.Clear();
    m_hints_video.Clear();

    m_startpts        = 0;

    m_pAudioCodec     = NULL;
    m_audio_render    = NULL;
    m_video_decoder   = NULL;

    m_audio_pkt       = NULL;
    m_video_pkt       = NULL;

    m_audio_codec_name = "";
    m_video_codec_name = "";

    // open file and start playing here.

    m_videoClock      = 0;
    m_audioClock      = 0;
    m_frametime       = 0;
    m_buffer_empty    = true;

    m_Passthrough     = IAudioRenderer::ENCODED_NONE;
    m_HWDecode        = false;
    m_use_hw_audio    = g_advancedSettings.m_omHWAudioDecode;

    m_dst_rect.SetRect(0, 0, 0, 0);

    m_filename = file.GetPath();
    
    if (!m_BcmHostDisplay.Load() || !m_BcmHost.Load())
      return false;

    memset(&m_tv_state, 0, sizeof(TV_GET_STATE_RESP_T));
    m_BcmHost.vc_tv_get_state(&m_tv_state);

    if(m_filename.find("3DSBS") != string::npos) {
      CLog::Log(LOGNOTICE, "3DSBS movie found");
      m_mode3d_sbs = true;
    }

    if(!m_omx_reader.Open(m_filename, false))
      return false;

    m_video_count   = m_omx_reader.VideoStreamCount();
    m_audio_count   = m_omx_reader.AudioStreamCount();
    m_chapter_count = m_omx_reader.GetChapterCount();

    m_hints_audio = m_omx_reader.GetAudioHints();
    m_hints_video = m_omx_reader.GetVideoHints();

    m_bMpeg = m_omx_reader.IsMpegVideo();

    m_ready.Reset();

    g_renderManager.PreInit();

    Create();

    if (!m_ready.WaitMSec(100))
    {
      CGUIDialogBusy* dialog = (CGUIDialogBusy*)g_windowManager.GetWindow(WINDOW_DIALOG_BUSY);
      dialog->Show();
      while(!m_ready.WaitMSec(1))
        g_windowManager.ProcessRenderLoop(false);
      dialog->Close();
    }
    // just in case process thread throws.
    //m_ready.Set();

    // Playback might have been stopped due to some error
    if (m_bStop || m_StopPlaying)
      return false;

    return true;
  }
  catch(...)
  {
    CLog::Log(LOGERROR, "%s - Exception thrown on open", __FUNCTION__);
    return false;
  }
}

bool COMXPlayer::CloseFile()
{
  CLog::Log(LOGDEBUG, "COMXPlayer::CloseFile");

  m_StopPlaying = true;

  CLog::Log(LOGDEBUG, "COMXPlayer: waiting for threads to exit");
  // wait for the main thread to finish up
  // since this main thread cleans up all other resources and threads
  // we are done after the StopThread call
  StopThread();
  
  m_omx_reader.Close();

  g_Windowing.InformVideoInfo(m_tv_state.width, m_tv_state.height, m_tv_state.frame_rate);

  m_BcmHostDisplay.Unload();
  m_BcmHost.Unload();

  CLog::Log(LOGDEBUG, "COMXPlayer: finished waiting");

  g_renderManager.UnInit();

  return true;
}

bool COMXPlayer::IsPlaying() const
{
  return !m_bStop;
}

void COMXPlayer::Pause()
{
  CSingleLock lock(m_csection);

  if(!m_av_clock)
    return;

  if (m_StopPlaying)
    return;

  if (m_paused == true)
  {
    // pause here
    m_callback.OnPlayBackResumed();
  }
  else
  {
    // unpause here
    m_callback.OnPlayBackPaused();
  }
  m_paused = !m_paused;
}

bool COMXPlayer::IsPaused() const
{
  return m_paused;
}

bool COMXPlayer::HasVideo() const
{
  return (m_video_count > 0);
}

bool COMXPlayer::HasAudio() const
{
  return (m_audio_count > 0);
}

void COMXPlayer::ToggleFrameDrop()
{
  CLog::Log(LOGDEBUG, "COMXPlayer::ToggleFrameDrop");
}

bool COMXPlayer::CanSeek()
{
  return m_bMpeg ? 0 : GetTotalTime() > 0;
}

void COMXPlayer::Seek(bool bPlus, bool bLargeStep)
{
  int chapter_index = GetChapter();
  if (bLargeStep)
  {
    // seek to next chapter
    if (bPlus && chapter_index < m_chapter_count)
    {
      SeekChapter(chapter_index + 1);
      return;
    }
    // seek to previous chapter
    if (!bPlus && chapter_index)
    {
      SeekChapter(chapter_index - 1);
      return;
    }
  }

  // update m_elapsed_ms and m_duration_ms.
  GetTime();
  GetTotalTime();

  int64_t seek_ms;
  if (g_advancedSettings.m_videoUseTimeSeeking &&
    (GetTotalTime() > (2 * g_advancedSettings.m_videoTimeSeekForwardBig)))
  {
    if (bLargeStep)
      seek_ms = bPlus ? g_advancedSettings.m_videoTimeSeekForwardBig : g_advancedSettings.m_videoTimeSeekBackwardBig;
    else
      seek_ms = bPlus ? g_advancedSettings.m_videoTimeSeekForward    : g_advancedSettings.m_videoTimeSeekBackward;
    // convert to milliseconds
    seek_ms *= 1000;
    seek_ms += m_elapsed_ms;
  }
  else
  {
    float percent;
    if (bLargeStep)
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForwardBig : g_advancedSettings.m_videoPercentSeekBackwardBig;
    else
      percent = bPlus ? g_advancedSettings.m_videoPercentSeekForward    : g_advancedSettings.m_videoPercentSeekBackward;
    percent /= 100.0f;
    percent += (float)m_elapsed_ms/(float)m_duration_ms;
    // convert to milliseconds
    seek_ms = m_duration_ms * percent;
  }

  // handle stacked videos, dvdplayer does it so we do it too.
  if (g_application.CurrentFileItem().IsStack() &&
    (seek_ms > m_duration_ms || seek_ms < 0))
  {
    CLog::Log(LOGDEBUG, "COMXPlayer::Seek: In mystery code, what did I do");
    g_application.SeekTime((seek_ms - m_elapsed_ms) * 0.001 + g_application.GetTime());
    // warning, don't access any object variables here as
    // the object may have been destroyed
    return;
  }

  if (seek_ms > m_duration_ms)
    seek_ms = m_duration_ms;

  g_infoManager.SetDisplayAfterSeek(100000);
  SeekTime(seek_ms);
  m_callback.OnPlayBackSeek((int)seek_ms, (int)(seek_ms - m_elapsed_ms));
  g_infoManager.SetDisplayAfterSeek();
}

bool COMXPlayer::SeekScene(bool bPlus)
{
  CLog::Log(LOGDEBUG, "COMXPlayer::SeekScene");
  return false;
}

void COMXPlayer::SeekPercentage(float fPercent)
{
  if (!m_duration_ms)
    return;

  SeekTime((int64_t)(m_duration_ms * fPercent / 100));
}

float COMXPlayer::GetPercentage()
{
  // update m_elapsed_ms and m_duration_ms.
  GetTime();
  GetTotalTime();
  if (m_duration_ms)
    return 100.0f * (float)m_elapsed_ms/(float)m_duration_ms;
  else
    return 0.0f;
}

float COMXPlayer::GetCachePercentage()
{
  CSingleLock lock(m_csection);
  return std::min(100.0, (double)(GetPercentage() + GetCacheLevel()));
}

void COMXPlayer::SetAVDelay(float fValue)
{
  // time offset in seconds of audio with respect to video
  m_audio_offset_ms = fValue * 1e3;
  // set a/v offset here
}

float COMXPlayer::GetAVDelay()
{
  return ((float)m_audio_offset_ms / 1e3);
}

void COMXPlayer::SetSubTitleDelay(float fValue)
{
  // time offset in seconds of subtitle with respect to playback
  m_subtitle_offset_ms = fValue * 1e3;
  // set sub offset here
}

float COMXPlayer::GetSubTitleDelay()
{
  return ((float)m_subtitle_offset_ms / 1e3);
}

void COMXPlayer::SetVolume(long nVolume)
{
  // nVolume is a milliBels from -6000 (-60dB or mute) to 0 (0dB or full volume)
  CSingleLock lock(m_csection);

  if(m_audio_render)
    m_audio_render->SetCurrentVolume(nVolume);
}

void COMXPlayer::GetAudioInfo(CStdString &strAudioInfo)
{
  std::ostringstream s;
    s << "kB/s:" << fixed << setprecision(2) << (double)m_hints_audio.bitrate / 1024.0;

  strAudioInfo.Format("Audio stream (%s) [%s]", m_audio_codec_name.c_str(), s.str());
}

void COMXPlayer::GetVideoInfo(CStdString &strVideoInfo)
{
  std::ostringstream s;
    s << "fr:"     << fixed << setprecision(3) << m_video_fps;
    s << ", Mb/s:" << fixed << setprecision(2) << (double)GetVideoBitrate() / (1024.0*1024.0);

  strVideoInfo.Format("Video stream (%s) [%s]", m_video_codec_name.c_str(), s.str());
}

void COMXPlayer::GetGeneralInfo(CStdString& strGeneralInfo)
{
  //CLog::Log(LOGDEBUG, "COMXPlayer::GetGeneralInfo");
}

int COMXPlayer::GetAudioStreamCount()
{
  return m_audio_count;
}

int COMXPlayer::GetAudioStream()
{
	return m_audio_index;
}

void COMXPlayer::GetAudioStreamName(int iStream, CStdString &strStreamName)
{
  GetAudioStreamLanguage(iStream, strStreamName);
}
 
void COMXPlayer::SetAudioStream(int SetAudioStream)
{
  CSingleLock lock(m_csection);

  if(m_omx_reader.SetAudioStream(SetAudioStream))
  {
    ResetStreams();
    m_audio_change = true;
  }
  m_audio_index = SetAudioStream;
}

void COMXPlayer::GetAudioStreamLanguage(int iStream, CStdString &strLanguage)
{
  CStdString language;

  strLanguage.Format("Undefined");

  if(m_omx_reader.GetAudioStreamLanguage(iStream, language))
  {
    if(language.GetLength() > 0)
      g_LangCodeExpander.Lookup( strLanguage, language.c_str() );
  }
}

int COMXPlayer::GetSubtitleCount()
{
	return m_subtitle_count;
}

int COMXPlayer::GetSubtitle()
{
	return m_subtitle_index;
}

void COMXPlayer::GetSubtitleName(int iStream, CStdString &strStreamName)
{
  //strStreamName = tcodec_language[iStream];
}

void COMXPlayer::SetSubtitle(int iStream)
{
  m_subtitle_index = iStream;
}

bool COMXPlayer::GetSubtitleVisible()
{
  return m_subtitle_show;
}

void COMXPlayer::SetSubtitleVisible(bool bVisible)
{
  m_subtitle_show = bVisible;
  g_settings.m_currentVideoSettings.m_SubtitleOn = bVisible;

  // show/hide subs here
}

int COMXPlayer::AddSubtitle(const CStdString& strSubPath)
{
  // dymamic add sub here
  return -1;
}

void COMXPlayer::Update(bool bPauseDrawing)
{
  g_renderManager.Update(bPauseDrawing);
}

void COMXPlayer::GetVideoRect(CRect& SrcRect, CRect& DestRect)
{
  g_renderManager.GetVideoRect(SrcRect, DestRect);
}

void COMXPlayer::SetVideoRect(const CRect &SrcRect, const CRect &DestRect)
{
  // check if destination rect or video view mode has changed
  if ((m_dst_rect != DestRect) || (m_view_mode != g_settings.m_currentVideoSettings.m_ViewMode))
  {
    m_dst_rect  = DestRect;
    m_view_mode = g_settings.m_currentVideoSettings.m_ViewMode;
  }
  else
  {
    return;
  }

  // might need to scale up m_dst_rect to display size as video decodes
  // to separate video plane that is at display size.
  CRect gui, display, dst_rect;
  RESOLUTION res = g_graphicsContext.GetVideoResolution();
  gui.SetRect(0, 0, g_settings.m_ResInfo[res].iWidth, g_settings.m_ResInfo[res].iHeight);
  display.SetRect(0, 0, g_settings.m_ResInfo[res].iWidth, g_settings.m_ResInfo[res].iHeight);
  
  dst_rect = m_dst_rect;
  if (gui != display)
  {
    float xscale = display.Width()  / gui.Width();
    float yscale = display.Height() / gui.Height();
    dst_rect.x1 *= xscale;
    dst_rect.x2 *= xscale;
    dst_rect.y1 *= yscale;
    dst_rect.y2 *= yscale;
  }

  if(m_video_decoder)
  {
    //xxx m_video_decoder->SetVideoRect(SrcRect, m_dst_rect);
  }
}

void COMXPlayer::GetVideoAspectRatio(float &fAR)
{
  fAR = g_renderManager.GetAspectRatio();
}

int COMXPlayer::GetChapterCount()
{
  return m_chapter_count;
}

int COMXPlayer::GetChapter()
{
  // returns a one based value.
  // if we have a chapter list, we need to figure out which chapter we are in.

  return m_omx_reader.GetChapter();
}

void COMXPlayer::GetChapterName(CStdString& strChapterName)
{
  m_omx_reader.GetChapterName(strChapterName);
}

int COMXPlayer::SeekChapter(int chapter_index)
{
  CSingleLock lock(m_csection);

  // chapter_index is a one based value.
  CLog::Log(LOGDEBUG, "COMXPlayer::SeekChapter:chapter_index(%d)", chapter_index);
  if(m_chapter_count > 1)
  {
    // Seek to the chapter.
    g_infoManager.SetDisplayAfterSeek(100000);

    m_omx_reader.SeekChapter(chapter_index, &m_startpts);
    ResetStreams();

    m_callback.OnPlayBackSeekChapter(chapter_index);
    g_infoManager.SetDisplayAfterSeek();
  }
  else
  {
    // we do not have a chapter list so do a regular big jump.
    if (chapter_index > 0)
      Seek(true,  true);
    else
      Seek(false, true);
  }

  return 0;
}

float COMXPlayer::GetActualFPS()
{
  return m_video_fps;
}

void COMXPlayer::SeekTime(__int64 seek_ms)
{
  CSingleLock lock(m_csection);

  int seek_flags = (seek_ms - m_elapsed_ms) < 0 ? AVSEEK_FLAG_BACKWARD : 0;

  m_omx_reader.SeekTime(seek_ms, seek_flags, &m_startpts);
  ResetStreams();
}

__int64 COMXPlayer::GetTime()
{
  return m_elapsed_ms;
}

int COMXPlayer::GetTotalTime()
{
	return m_duration_ms / 1000;
}

int COMXPlayer::GetAudioBitrate()
{
  return m_hints_audio.bitrate;
}
int COMXPlayer::GetVideoBitrate()
{
  return (int)m_videoStats.GetBitrate();
}

int COMXPlayer::GetSourceBitrate()
{
  return m_omx_reader.GetSourceBitrate();
}

int COMXPlayer::GetChannels()
{
  return m_hints_audio.channels;
}

int COMXPlayer::GetBitsPerSample()
{
  return m_hints_audio.bitspersample;
}

int COMXPlayer::GetSampleRate()
{
  return m_hints_audio.samplerate;
}

CStdString COMXPlayer::GetAudioCodecName()
{
  return m_audio_codec_name;
}

CStdString COMXPlayer::GetVideoCodecName()
{
  return m_video_codec_name;
}

int COMXPlayer::GetPictureWidth()
{
  return m_hints_video.width;
}

int COMXPlayer::GetPictureHeight()
{
  return m_hints_video.height;
}

bool COMXPlayer::GetStreamDetails(CStreamDetails &details)
{
  unsigned int i;
  bool retVal = false;
  details.Reset();
  
  for(i = 0; i < m_video_count; i++)
  {
    CStreamDetailVideo *p = new CStreamDetailVideo();
    COMXStreamInfo hints = m_omx_reader.GetVideoHints(i);

    p->m_iWidth   = hints.width;
    p->m_iHeight  = hints.height;
    p->m_fAspect = hints.aspect;
    p->m_iDuration = m_duration_ms;

    // finally, calculate seconds
    if (p->m_iDuration > 0)
      p->m_iDuration = p->m_iDuration / 1000;

    details.AddStream(p);
    retVal = true;
  }

  for(i = 0; i < m_audio_count; i++)
  {
    CStreamDetailAudio *p = new CStreamDetailAudio();
    COMXStreamInfo hints = m_omx_reader.GetAudioHints(i);
    CStdString strLanguage;
    CStdString strCodec;

    p->m_iChannels  = hints.channels;
    GetAudioStreamLanguage(i, strLanguage);
    p->m_strLanguage = strLanguage;

    p->m_strCodec = m_omx_reader.GetVideoCodecName(i);

    details.AddStream(p);
    retVal = true;
  }

  // TODO: here we would have subtitles

  return retVal;
}

void COMXPlayer::ToFFRW(int iSpeed)
{
  if (m_StopPlaying)
    return;

  if(!m_av_clock)
    return;

  if(iSpeed < OMX_PLAYSPEED_PAUSE)
    return;

  m_omx_reader.SetSpeed(iSpeed);

  if(m_av_clock->PlaySpeed() != OMX_PLAYSPEED_PAUSE && iSpeed == OMX_PLAYSPEED_PAUSE)
    m_paused = true;
  else if(m_av_clock->PlaySpeed() == OMX_PLAYSPEED_PAUSE && iSpeed != OMX_PLAYSPEED_PAUSE)
    m_paused = false;

  m_av_clock->Speed(iSpeed);
}

bool COMXPlayer::GetCurrentSubtitle(CStdString& strSubtitle)
{
  strSubtitle = "";

  //strSubtitle = subtitle_text;
  return !strSubtitle.IsEmpty();
}
  
CStdString COMXPlayer::GetPlayerState()
{
  return "";
}

bool COMXPlayer::SetPlayerState(CStdString state)
{
  return false;
}

CStdString COMXPlayer::GetPlayingTitle()
{
  //return video_title;
  return "";
}

int COMXPlayer::GetCacheLevel() const
{
  return 0;
}

void COMXPlayer::OnStartup()
{
}

void COMXPlayer::OnExit()
{
  m_bStop = true;
  // if we didn't stop playing, advance to the next item in xbmc's playlist
  if(m_options.identify == false)
  {
    if (m_StopPlaying)
      m_callback.OnPlayBackStopped();
    else
      m_callback.OnPlayBackEnded();
  }


  m_ready.Set();
}

void COMXPlayer::Process()
{
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
    CLog::Log(LOGDEBUG, "COMXPlayer: SetThreadPriority failed");

  int                 result            = -1;
  int                 m_video_index_use = -1;
  int                 m_audio_index_use = -1;

  m_video_count   = m_omx_reader.VideoStreamCount();
  m_audio_count   = m_omx_reader.AudioStreamCount();
  m_chapter_count = m_omx_reader.GetChapterCount();

  m_hints_audio = m_omx_reader.GetAudioHints();
  m_hints_video = m_omx_reader.GetVideoHints();

  if(!m_av_clock->Initialize(m_video_count, m_audio_count))
    goto do_exit;

  if(!m_video_count && !m_audio_count)
    goto do_exit;

  if(m_video_count && !OpenVideoDecoder(m_hints_video))
    goto do_exit;

  m_dst_rect.SetRect(0, 0, 0, 0);
  //if(m_video_decoder)
  //  m_video_decoder->SetVideoRect(m_dst_rect, m_dst_rect);

  m_Passthrough = IsPassthrough(m_hints_audio);
  if(!m_Passthrough && m_use_hw_audio)
    m_HWDecode = COMXAudio::HWDecode(m_hints_audio.codec);

  m_av_clock->StateExecute();

  m_duration_ms = m_omx_reader.GetDuration();

  //CLog::Log(LOGDEBUG, "COMXPlayer: Thread started");
  try
  {
    m_speed = DVD_PLAYSPEED_NORMAL;
    m_callback.OnPlayBackSpeedChanged(m_speed);

    // starttime has units of seconds (SeekTime will start playback)
    if (m_options.starttime > 0)
      SeekTime(m_options.starttime * 1000);
    SetVolume(g_settings.m_nVolumeLevel);
    SetAVDelay(m_audio_offset_ms);

    // at this point we should know all info about audio/video stream.
    // we are done initializing now, set the readyevent which will
    if (m_video_count)
    {
      // turn on/off subs
      SetSubtitleVisible(g_settings.m_currentVideoSettings.m_SubtitleOn);
      SetSubTitleDelay(m_subtitle_offset_ms);

      // setup renderer for bypass. This tell renderer to get out of the way as
      // hw decoder will be doing the actual video rendering in a video plane
      // that is under the GUI layer.
      int width  = GetPictureWidth();
      int height = GetPictureHeight();
      double fFrameRate = GetActualFPS();
      unsigned int flags = 0;

      flags |= CONF_FLAGS_FORMAT_BYPASS;
      flags |= CONF_FLAGS_FULLSCREEN;
      CLog::Log(LOGDEBUG,"%s - change configuration. %dx%d. framerate: %4.2f. format: BYPASS",
        __FUNCTION__, width, height, fFrameRate);

      if(!g_renderManager.Configure(m_video_width, m_video_height,
        m_video_width, m_video_height, m_video_fps, flags, 0))
      {
        CLog::Log(LOGERROR, "%s - failed to configure renderer", __FUNCTION__);
      }
      if (!g_renderManager.IsStarted())
      {
        CLog::Log(LOGERROR, "%s - renderer not started", __FUNCTION__);
      }
      g_Windowing.InformVideoInfo(width, height, (int)(fFrameRate+0.5), m_mode3d_sbs);
    }

    if (m_options.identify == false)
      m_callback.OnPlayBackStarted();

    // drop CGUIDialogBusy, and release the hold in OpenFile
    m_ready.Set();

    m_videoStats.Start();

    CSingleLock lock(m_csection);
    m_csection.unlock();

    while (!m_bStop && !m_StopPlaying)
    {
      if(m_paused)
      {
        if(!m_av_clock->IsPaused())
          m_av_clock->Pause();

        OMXSleep(2);
        continue;
      }
      else if(!m_buffer_empty && !m_paused)
      {
        if(m_av_clock->IsPaused())
          m_av_clock->Resume();
      }

      m_csection.lock();

      if(m_audio_change && m_audio_count)
      {
        // audio stream changed. trigger reinit
        m_av_clock->Pause();

        CloseAudioDecoder();
        CloseAudioCodec();

        m_hints_audio     = m_omx_reader.GetAudioHints();
        m_hints_video     = m_omx_reader.GetVideoHints();

        if(!OpenAudioCodec(m_hints_audio))
        {
          CLog::Log(LOGERROR, "%s - failed to open audio codec", __FUNCTION__);
          m_csection.unlock();
          goto do_exit;
        }

        if(m_Passthrough)
          m_Passthrough = IsPassthrough(m_hints_audio);

        if(!m_Passthrough && m_use_hw_audio)
          m_HWDecode = COMXAudio::HWDecode(m_hints_audio.codec);

        if(!OpenAudioDecoder(m_hints_audio))
        {
          CLog::Log(LOGERROR, "%s - failed to open audio decoder", __FUNCTION__);
          m_csection.unlock();
          goto do_exit;
        }

        m_av_clock->StateExecute();
        m_av_clock->Reset();
        m_av_clock->Resume();

        m_audio_change = false;
      }

      /* when the audio buffer runns under 0.1 seconds we buffer up */
      if(m_audio_render && m_audio_render->GetDelay() < 0.1f)
      {
        m_buffer_empty = true;
        if(!m_av_clock->IsPaused())
          m_av_clock->Pause();
      }

      /* buffering once after seek */
      if(m_buffer_empty)
      {
        if(m_audio_render)
        {
          if(m_audio_render->GetDelay() > (AUDIO_BUFFER_SECONDS - 0.25f))
          {
            if(m_av_clock->IsPaused())
              m_av_clock->Resume();
            m_buffer_empty = false;
          }
        }
        if(m_video_decoder)
        {
          if(m_video_decoder->GetFreeSpace() < (m_video_decoder->GetInputBufferSize() * 0.25) && m_omx_reader.GetVideoPacketsFree() == 0)
          {
            if(m_av_clock->IsPaused())
              m_av_clock->Resume();
            m_buffer_empty = false;
          }
        }
      }

      if(!m_video_pkt)
      {
        m_video_pkt = m_omx_reader.GetVideoPacket();
      }
      else
      {
        if((unsigned long)m_video_decoder->GetFreeSpace() > m_video_pkt->size)
        {
          if (m_video_pkt->dts == DVD_NOPTS_VALUE && m_video_pkt->pts == DVD_NOPTS_VALUE)
            m_video_pkt->pts = m_videoClock;
          else if (m_video_pkt->pts == DVD_NOPTS_VALUE)
            m_video_pkt->pts = m_video_pkt->dts;

          if(m_video_pkt->pts != DVD_NOPTS_VALUE)
            m_videoClock = m_video_pkt->pts;

          if(m_bMpeg)
            m_video_decoder->Decode(m_video_pkt->data, m_video_pkt->size, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
          else
            m_video_decoder->Decode(m_video_pkt->data, m_video_pkt->size, 
                                    m_videoClock + (m_audio_offset_ms * 1000), 
                                    m_videoClock + (m_audio_offset_ms * 1000));

          m_videoClock += m_frametime;
          m_av_clock->SetPTS(m_videoClock);
          m_videoStats.AddSampleBytes(m_video_pkt->size);

          if(!m_audio_render && m_video_decoder)
          {
            printf("V : %8.02f %8d %8d                              \r", m_videoClock / DVD_TIME_BASE, m_video_decoder->GetInputBufferSize(),
                 m_video_decoder->GetFreeSpace());
          }

          if(m_audio_count == 0)
            usleep(m_frametime - 2000);

          m_omx_reader.FreePacket(m_video_pkt);
          m_video_pkt = NULL;
        }
      }

      if(!m_audio_pkt)
      {
        m_audio_pkt = m_omx_reader.GetAudioPacket();
      }
      else
      {
        if(m_audio_pkt->dts != DVD_NOPTS_VALUE)
          m_audioClock = m_audio_pkt->dts;

        m_av_clock->SetPTS(m_audioClock);

        if((unsigned long)m_audio_render->GetSpace() > m_audio_pkt->size)
        {
          const uint8_t *data_dec = m_audio_pkt->data;
          int            data_len = m_audio_pkt->size;

          if(!m_Passthrough && !m_HWDecode)
          {
            while(data_len > 0)
            {
              int len = m_pAudioCodec->Decode((BYTE *)data_dec, data_len);
              if( (len < 0) || (len >  data_len) )
              {
                m_pAudioCodec->Reset();
                break;
              }

              data_dec+= len;
              data_len -= len;

              uint8_t *decoded;
              int decoded_size = m_pAudioCodec->GetData(&decoded);

              if(decoded_size <=0)
                continue;

              int ret = 0;

              if(m_bMpeg)
                ret = m_audio_render->AddPackets(decoded, decoded_size, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
              else
                ret = m_audio_render->AddPackets(decoded, decoded_size, m_audioClock, m_audioClock);

              if(ret != decoded_size)
              {
                printf("error ret %d decoded_size %d\n", ret, decoded_size);
              }
              int n = (m_hints_audio.channels * m_hints_audio.bitspersample * m_hints_audio.samplerate)>>3;
              if (n > 0 && m_audioClock != DVD_NOPTS_VALUE)
              {
                m_audioClock += ((double)decoded_size * DVD_TIME_BASE) / n;
              }
            }
          }
          else
          {
            if(m_bMpeg)
              m_audio_render->AddPackets(m_audio_pkt->data, m_audio_pkt->size, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
            else
              m_audio_render->AddPackets(m_audio_pkt->data, m_audio_pkt->size, m_audioClock, m_audioClock);
          }

          if(m_audio_render && m_video_decoder)
          {
            printf("V : %8.02f %8d %8d A : %8.02f %8.02f                             \r",
                m_videoClock / DVD_TIME_BASE, m_video_decoder->GetInputBufferSize(),
                m_video_decoder->GetFreeSpace(), m_audioClock / DVD_TIME_BASE, m_audio_render->GetDelay());
          }
          else if(m_audio_render)
          {
            printf("A : %8.02f %8.02f                             \r",
                m_audioClock / DVD_TIME_BASE, m_audio_render->GetDelay());
          }

          m_omx_reader.FreePacket(m_audio_pkt);
          m_audio_pkt = NULL;
        }
      }

      if(m_audioClock != AV_NOPTS_VALUE)
        m_elapsed_ms = m_audioClock / 1000;
      else if(m_videoClock != AV_NOPTS_VALUE)
        m_elapsed_ms = m_videoClock / 1000;
      else
        m_elapsed_ms = 0;

      m_csection.unlock();

      if(m_omx_reader.IsEof())
        break;
    }
  }
  catch(...)
  {
    CLog::Log(LOGERROR, "COMXPlayer::Process: Exception thrown");
  }

do_exit:

  m_av_clock->Stop();

  CloseAudioDecoder();
  CloseVideoDecoder();
  CloseAudioCodec();

  if(m_audio_pkt)
  {
    m_omx_reader.FreePacket(m_audio_pkt);
    m_audio_pkt = NULL;
  }

  if(m_video_pkt)
  {
    m_omx_reader.FreePacket(m_video_pkt);
    m_video_pkt = NULL;
  }

  m_bStop = m_StopPlaying = true;
}

void COMXPlayer::RegisterAudioCallback(IAudioCallback* pCallback)
{
  if (m_audio_render)
    m_audio_render->RegisterAudioCallback(pCallback);
}

void COMXPlayer::UnRegisterAudioCallback()
{
  if (m_audio_render)
    m_audio_render->UnRegisterAudioCallback();
}

void COMXPlayer::DoAudioWork()
{
  if (m_audio_render)
    m_audio_render->DoAudioWork();
}

#endif