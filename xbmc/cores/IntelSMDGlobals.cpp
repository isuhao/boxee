#include "system.h"

#ifdef HAS_INTEL_SMD

/*
 * Boxee
 * Copyright (c) 2009, Boxee Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "Settings.h"
#include "GUISettings.h" // for AUDIO_*
#include "IntelSMDGlobals.h"
#include <ismd_vidpproc.h>
#include "utils/log.h"
#include "HalServices.h"
#include "lib/libBoxee/bxoemconfiguration.h"

#ifndef DVD_TIME_BASE
#define DVD_TIME_BASE 1000000
#endif
#ifndef DVD_NOPTS_VALUE
#define DVD_NOPTS_VALUE    (-1LL<<52) // should be possible to represent in both double and __int64
#endif

CIntelSMDGlobals g_IntelSMDGlobals;

CIntelSMDGlobals::CIntelSMDGlobals()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::CIntelSMDGlobals");

  m_main_clock = -1;
  m_demux = -1;
  m_demux_input_port = -1;
  m_demux_video_port = -1;
  m_demux_audio_port = -1;
  m_demux_ves_filter_handle = -1;
  m_demux_aes_filter_handle = -1;
  m_audioProcessor = -1;
  m_audioDev = -1;
  m_audioDevNonTimed = -1;
  m_audio_inputPort = -1;
  m_audio_inputPortNonTimed = -1;
  m_audio_output[0] = -1;
  m_audio_output[1] = -1;
  m_audio_output[2] = -1;
  m_audio_output_device_added[0] = false;
  m_audio_output_device_added[1] = false;
  m_audio_output_device_added[2] = false;
  m_audio_output_count = -1;
  m_viddec = -1;
  m_viddec_input_port = -1;
  m_viddec_output_port = -1;
  m_viddec_user_data_port = -1;
  m_video_proc = -1;
  m_video_render = -1;
  m_video_input_port_proc = -1;
  m_video_input_port_renderer = -1;
  m_video_output_port_renderer = -1;
  m_video_output_port_proc = -1;
  m_video_codec = ISMD_CODEC_TYPE_INVALID;
  m_viddec_user_data_event = -1;

  m_base_time = 0;
  m_pause_base_time = 0;
  m_RenderState = ISMD_DEV_STATE_INVALID;
  m_audio_start_pts = ISMD_NO_PTS;
  m_video_start_pts = ISMD_NO_PTS;

  m_bFlushFlag = false;

  m_bDemuxToVideo = false;
  m_bDemuxToAudio = false;

  memset((void*) m_HDMIBitstreamCodecs, 0, 6 * 2 * sizeof(int));
  memset((void*) m_HWDecodeCodecs, 0, HW_AUDIO_DECODING_NUM * 2 * sizeof(int));

  ResetClock();
}

CIntelSMDGlobals::~CIntelSMDGlobals()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::~CIntelSMDGlobals");

  DestroyMainClock();
  DeleteAudioDevice(true);
  DeleteAudioDevice(false);
  //DeleteAudioProcessor();
}

bool CIntelSMDGlobals::CreateMainClock()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::CreateMainClock");

  ismd_result_t ret;

  CSingleLock lock(m_Lock);

  if (m_main_clock != -1)
    return true;

  ret = ismd_clock_alloc(ISMD_CLOCK_TYPE_FIXED, &m_main_clock);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "ERROR: CIntelSMDGlobals::CreateMainClock CLOCK ALLOC FAIL .....");
    return false;
  }

  if (!SetClockPrimary())
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::CreateMainClock SetClockPrimary failed");
    return false;
  }

  return true;
}

void CIntelSMDGlobals::DestroyMainClock()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::DestroyMainClock");

  CSingleLock lock(m_Lock);

  if (m_main_clock != -1)
    ismd_clock_free(m_main_clock);

  m_main_clock = -1;
}

bool CIntelSMDGlobals::SetClockPrimary()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::SetClockPrimary");

  ismd_result_t ret;

  if (m_main_clock == -1)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SetClockPrimary  m_main_clock == -1");
    return false;
  }

  ret = ismd_clock_make_primary(m_main_clock);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(
        LOGERROR,
        "ERROR: CIntelSMDGlobals::SetClockPrimary ismd_clock_make_primary failed %d",
        ret);
    return false;
  }

  return true;
}

ismd_time_t CIntelSMDGlobals::GetCurrentTime()
{
  ismd_result_t ret;
  ismd_time_t current;

  CSingleLock lock(m_Lock);

  if (m_main_clock == -1)
  {
    CLog::Log(LOGWARNING, "CIntelSMDGlobals::GetCurrentTime main clock = -1");
    return 0;
  }

  ret = ismd_clock_get_time(m_main_clock, &current);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::GetCurrentTime ismd_clock_get_time failed %d",
        ret);
    return 0;
  }

  return current;
}

void CIntelSMDGlobals::SetBaseTime(ismd_time_t time)
{
  CLog::Log(LOGINFO, "Setting base time %.2f", IsmdToDvdPts(time) / 1000000);
  m_base_time = time + 45000;
}

bool CIntelSMDGlobals::SetCurrentTime(ismd_time_t time)
{
  ismd_result_t ret;

  CSingleLock lock(m_Lock);

  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::SetCurrentTime");

  if (m_main_clock == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::SetCurrentTime main clock = -1");
    return false;
  }

  ret = ismd_clock_set_time(m_main_clock, time);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SetCurrentTime ismd_clock_set_time failed %d", ret);
    return false;
  }

  return true;
}

void CIntelSMDGlobals::PauseClock()
{
  CSingleLock lock(m_Lock);

  m_pause_base_time = GetBaseTime();
  m_pause_cur_time = GetCurrentTime();
}

void CIntelSMDGlobals::ResumeClock()
{
  CSingleLock lock(m_Lock);

  if (m_pause_base_time == 0)
  {
    if (m_base_time != 0)
      m_pause_base_time = GetBaseTime();
    else
      m_pause_base_time = GetCurrentTime();
  }
  if (m_pause_cur_time == 0)
    m_pause_cur_time = GetCurrentTime();

  ismd_time_t offset = GetCurrentTime() - m_pause_cur_time;
  m_base_time = m_pause_base_time + offset;

  m_pause_base_time = 0;
  m_pause_cur_time = 0;
}

void CIntelSMDGlobals::ResetClock()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::ResetClock");

  m_base_time = 0;
  m_pause_base_time = 0;
  m_pause_cur_time = 0;
  m_audio_start_pts = ISMD_NO_PTS;
  m_video_start_pts = ISMD_NO_PTS;
  m_bFlushFlag = false;
}

ismd_pts_t CIntelSMDGlobals::DvdToIsmdPts(double pts)
{
  ismd_pts_t ismd_pts = ISMD_NO_PTS;

  if (pts != DVD_NOPTS_VALUE
    )
    ismd_pts = (ismd_pts_t) (pts / DVD_TIME_BASE * SMD_CLOCK_FREQ);

  return ismd_pts;
}

double CIntelSMDGlobals::IsmdToDvdPts(ismd_pts_t pts)
{
  double dvd_pts = DVD_NOPTS_VALUE;

  if (pts != ISMD_NO_PTS)
    dvd_pts = (double) ((double) pts / SMD_CLOCK_FREQ * DVD_TIME_BASE);

  return dvd_pts;
}

bool CIntelSMDGlobals::SetAudioStartPts(ismd_pts_t pts)
{
  CSingleLock lock(m_Lock);

  if (pts == ISMD_NO_PTS)
  {
    CLog::Log(LOGWARNING, "CIntelSMDGlobals::SetAudioStartPts got ISMD_NO_PTS");
    return false;
  }

  if (pts < 0 && pts != ISMD_NO_PTS)
    pts = 0;

  CLog::Log(LOGINFO, "CIntelSMDGlobals::SetAudioStartPts %.2f ",
      IsmdToDvdPts(pts) / 1000000.0);

  m_audio_start_pts = pts;
}

bool CIntelSMDGlobals::SetVideoStartPts(ismd_pts_t pts)
{
  CSingleLock lock(m_Lock);

  if (pts == ISMD_NO_PTS)
  {
    CLog::Log(LOGWARNING, "CIntelSMDGlobals::SetVideoStartPts got ISMD_NO_PTS");
    return false;
  }

  if (pts < 0 && pts != ISMD_NO_PTS)
    pts = 0;

  CLog::Log(LOGINFO, "CIntelSMDGlobals::SetVideoStartPts %.2f ",
      IsmdToDvdPts(pts) / 1000000.0);

  m_video_start_pts = pts;
}

ismd_pts_t CIntelSMDGlobals::GetAudioCurrentTime()
{
  ismd_pts_t start = GetAudioStartPts();

  if (start == ISMD_NO_PTS || (GetCurrentTime() < GetBaseTime()))
    return ISMD_NO_PTS;

  return start + GetCurrentTime() - GetBaseTime();
}

ismd_pts_t CIntelSMDGlobals::GetVideoCurrentTime()
{
  ismd_pts_t start = GetVideoStartPts();

  if (start == ISMD_NO_PTS || (GetCurrentTime() < GetBaseTime()))
    return ISMD_NO_PTS;

  return start + GetCurrentTime() - GetBaseTime();
}

ismd_pts_t CIntelSMDGlobals::GetAudioPauseCurrentTime()
{
  ismd_pts_t start = GetAudioStartPts();

  if (start == ISMD_NO_PTS || (GetCurrentTime() < GetBaseTime()))
    return ISMD_NO_PTS;

  return start + GetPauseCurrentTime() - GetBaseTime();
}

ismd_pts_t CIntelSMDGlobals::GetVideoPauseCurrentTime()
{
  ismd_pts_t start = GetVideoStartPts();

  if (start == ISMD_NO_PTS || (GetCurrentTime() < GetBaseTime()))
    return ISMD_NO_PTS;

  return start + GetPauseCurrentTime() - GetBaseTime();
}

ismd_pts_t CIntelSMDGlobals::Resync(bool bDisablePtsCorrection)
{
  ismd_pts_t audioStart;
  ismd_pts_t videoStart;
  ismd_pts_t deltaPTS;
  ismd_pts_t eps = 10000;
  ismd_pts_t newPTS;

  audioStart = GetAudioStartPts();
  videoStart = GetVideoStartPts();

  if (audioStart == ISMD_NO_PTS)
    return ISMD_NO_PTS;

  if (videoStart == ISMD_NO_PTS)
    return audioStart;

  deltaPTS = abs(videoStart - audioStart);

  if (deltaPTS == 0)
    return videoStart;

  if (bDisablePtsCorrection)
    CLog::Log(LOGINFO, "CIntelSMDGlobals::Resync no pts correction");

  if (deltaPTS > eps && deltaPTS < 90000 * 15 && !bDisablePtsCorrection)
  {
    if (videoStart < audioStart)
      newPTS = videoStart + deltaPTS / 4;
    else
      newPTS = videoStart - deltaPTS / 4;
  }
  else
  {
    newPTS = audioStart;
  }

  return newPTS;
}

void CIntelSMDGlobals::CreateStartPacket(ismd_pts_t start_pts,
    ismd_buffer_handle_t buffer_handle)
{
  CSingleLock lock(m_Lock);

  ismd_newsegment_tag_t newsegment_data;
  ismd_es_buf_attr_t *buf_attrs;
  ismd_buffer_descriptor_t buffer_desc;
  ismd_result_t ismd_ret;

  if (buffer_handle == 0)
  {
    CLog::Log(LOGWARNING, "CIntelSMDGlobals::CreateStartPacket buffer_handle = NULL");
    return;
  }

  //printf("***Create start packet to Video renderer. pts = %.2f", IsmdToDvdPts(start_pts) / 1000000);

  newsegment_data.linear_start = 0;
  newsegment_data.start = start_pts;
  newsegment_data.stop = ISMD_NO_PTS;
  newsegment_data.requested_rate = ISMD_NORMAL_PLAY_RATE;
  newsegment_data.applied_rate = ISMD_NORMAL_PLAY_RATE;
  newsegment_data.rate_valid = true;
  newsegment_data.segment_position = ISMD_NO_PTS;

  ismd_ret = ismd_tag_set_newsegment_position(buffer_handle, newsegment_data);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SendStartPacket ismd_tag_set_newsegment failed");
    return;
  }
}

void CIntelSMDGlobals::SendStartPacket(ismd_pts_t start_pts,
    ismd_port_handle_t port, ismd_buffer_handle_t buffer)
{
  CSingleLock lock(m_Lock);

  ismd_newsegment_tag_t newsegment_data;
  ismd_es_buf_attr_t *buf_attrs;
  ismd_buffer_handle_t buffer_handle;
  ismd_buffer_descriptor_t buffer_desc;
  ismd_result_t ismd_ret;
  //create a carrier buffer for the new segment
  if (buffer != 0)
    buffer_handle = buffer;
  else
  {
    ismd_ret = ismd_buffer_alloc(0, &buffer_handle);
    if (ismd_ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDGlobals::SendStartPacket ismd_buffer_alloc failed");
      return;
    }
  }

  /*
   printf("***Sending start packet to ");
   if(port == m_viddec_input_port)
   printf("Video decoder. pts = %.2f", IsmdToDvdPts(start_pts) / 1000000);
   else if(port == m_video_input_port_proc)
   printf("Video proc. pts = %.2f", IsmdToDvdPts(start_pts) / 1000000);
   else if(port == m_audio_inputPort)
   printf("Audio device. pts = %.2f", IsmdToDvdPts(start_pts) / 1000000);
   else
   printf("Unknown input. pts = %.2f", IsmdToDvdPts(start_pts) / 1000000);
   */

  newsegment_data.linear_start = 0;
  newsegment_data.start = start_pts;
  newsegment_data.stop = ISMD_NO_PTS;
  newsegment_data.requested_rate = ISMD_NORMAL_PLAY_RATE;
  newsegment_data.applied_rate = ISMD_NORMAL_PLAY_RATE;
  newsegment_data.rate_valid = true;
  newsegment_data.segment_position = ISMD_NO_PTS;

  ismd_ret = ismd_tag_set_newsegment_position(buffer_handle, newsegment_data);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SendStartPacket ismd_tag_set_newsegment failed");
    return;
  }

  ismd_ret = ismd_port_write(port, buffer_handle);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SendStartPacket ismd_port_write failed");
    return;
  }
}

bool CIntelSMDGlobals::CreateDemuxer()
{
  ismd_result_t ismd_ret;
  unsigned output_buf_size = 0;

  ismd_port_handle_t dummy_port_handle;

  if(m_demux != -1)
  {
    CLog::Log(LOGWARNING, "CIntelSMDGlobals::CreateDemuxer demux is already open, closing");
    DeleteDemuxer();
  }

  ismd_ret = ismd_demux_stream_open(
      ISMD_DEMUX_STREAM_TYPE_MPEG2_TRANSPORT_STREAM, &m_demux,
      &m_demux_input_port);

  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer ismd_demux_stream_open failed %d", ismd_ret);
    return false;
  }

  // create the video filter
  output_buf_size = 32 * 1024;
  ismd_ret = ismd_demux_filter_open(m_demux, ISMD_DEMUX_OUTPUT_ES,
      output_buf_size, &m_demux_ves_filter_handle, &m_demux_video_port);

  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer failed at ismd_demux_filter_open %d", ismd_ret);
    return false;
  }

  // create the audio filter
  output_buf_size = 8 * 1024;
  ismd_ret = ismd_demux_filter_open(m_demux, ISMD_DEMUX_OUTPUT_ES,
      output_buf_size, &m_demux_aes_filter_handle, &m_demux_audio_port);

  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer failed at ismd_demux_filter_open %d", ismd_ret);
    return false;
  }

  // Clock
  ismd_ret = ismd_dev_set_clock(m_demux, m_main_clock);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer: failed at ismd_dev_set_clock %d", ismd_ret);
    return false;
  }

  /*
  ismd_ret = ismd_demux_stream_use_arrival_time(m_demux, false);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer: failed at ismd_demux_stream_use_arrival_time %d", ismd_ret);
    return false;
  }
  */

  ismd_ret = ismd_dev_set_state(m_demux, ISMD_DEV_STATE_PAUSE);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer:failed at ismd_dev_set_state %d", ismd_ret);
    return false;
  }

  ismd_ret = ismd_demux_enable_leaky_filters(m_demux);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateDemuxer:failed at ismd_demux_enable_leaky_filters %d", ismd_ret);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::DeleteDemuxer()
{
  ismd_result_t res;

  if (m_demux == -1)
    return true;

  SetDemuxDeviceState(ISMD_DEV_STATE_STOP);
  FlushDemuxer();

  if (m_demux_ves_filter_handle != -1)
    ismd_demux_filter_close(m_demux, m_demux_ves_filter_handle);

  if (m_demux_aes_filter_handle != -1)
    ismd_demux_filter_close(m_demux, m_demux_aes_filter_handle);

  ismd_dev_close(m_demux);

  m_demux = -1;
  m_demux_ves_filter_handle = -1;
  m_demux_aes_filter_handle = -1;
  m_demux_input_port = -1;

  m_demux_video_port = -1;
  m_demux_audio_port = -1;

  m_bDemuxToAudio = false;
  m_bDemuxToVideo = false;

  return true;
}

bool CIntelSMDGlobals::FlushDemuxer()
{
  CSingleLock lock(m_Lock);
  ismd_result_t ret;

  SetDemuxDeviceState(ISMD_DEV_STATE_PAUSE);

  if (m_demux != -1)
    ret = ismd_dev_flush(m_demux);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::FlushDemuxer ismd_dev_flush failed %d", ret);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::SetDemuxDeviceState(ismd_dev_state_t state)
{
  ismd_result_t ret;

  if (state == ISMD_DEV_STATE_INVALID)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SetDemuxDeviceState setting device to invalid");
    return true;
  }

  CSingleLock lock(m_Lock);

  if (m_demux == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::SetDemuxDeviceState device = -1");
    return false;
  }

  ret = ismd_dev_set_state(m_demux, state);
  if (ret != ISMD_SUCCESS)
    CLog::Log(
        LOGWARNING,
        "CIntelSMDGlobals::SetDemuxDeviceState ismd_dev_set_state %d on demux device failed %d",
        state, ret);

  CLog::Log(LOGDEBUG, "SetDemuxDeviceState state %d ret %d", state, ret);

  return true;
}

bool CIntelSMDGlobals::ConnectDemuxerToAudio()
{
  ismd_result_t ismd_ret;
  ismd_port_handle_t demux_output_port;

  CLog::Log(LOGDEBUG, "ConnectDemuxerToAudio");

  ismd_ret = ismd_demux_filter_get_output_port(m_demux, m_demux_aes_filter_handle, &demux_output_port);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConnectDemuxerToVideo failed at ismd_port_connect, ts_vid to viddec %d", ismd_ret);
    return false;
  }

  ismd_ret = ismd_port_connect(demux_output_port, GetAudioInput(true));
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "failed at ismd_port_connect, ts_aud to audio");
    return false;
  }

  m_bDemuxToAudio = true;

  return true;
}

bool CIntelSMDGlobals::ConnectDemuxerToVideo()
{
  ismd_result_t ismd_ret;
  ismd_port_handle_t demux_output_port;

  CLog::Log(LOGDEBUG, "ConnectDemuxerToVideo");

  if(GetVidDecInput() == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConnectDemuxerToVideo failed GetVidDecInput is -1\n");
    return false;
  }

  ismd_ret = ismd_demux_filter_get_output_port(m_demux, m_demux_ves_filter_handle, &demux_output_port);
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConnectDemuxerToVideo failed at ismd_port_connect, ts_vid to viddec %d", ismd_ret);
    return false;
  }

  ismd_ret = ismd_port_connect(demux_output_port, GetVidDecInput());
  if (ismd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConnectDemuxerToVideo failed at ismd_port_connect, ts_vid to viddec\n");
    return false;
  }

  m_bDemuxToVideo = true;

  return true;
}

bool CIntelSMDGlobals::SetDemuxDeviceBaseTime(ismd_time_t time)
{
  //printf("%s", __FUNCTION__);
  CLog::Log(LOGDEBUG, "SetDemuxDeviceBaseTime base time %.2f",
      IsmdToDvdPts(time) / 1000000);

  ismd_result_t ret;
  ismd_dev_state_t state = ISMD_DEV_STATE_INVALID;

  CSingleLock lock(m_Lock);

  if (m_demux != -1)
  {
    ismd_dev_get_state(m_demux, &state);
    if (state == ISMD_DEV_STATE_PLAY)
    {
      CLog::Log(LOGWARNING,
          "CIntelSMDGlobals::SetDemuxDeviceBaseTime device is running. Ignoring request");
      return true;
    }

    ret = ismd_dev_set_stream_base_time(m_demux, time);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::SetDemuxDeviceBaseTime ismd_dev_set_stream_base_time for demux device failed");
    }
  }

  return true;
}

bool CIntelSMDGlobals::CreateVideoDecoder(ismd_codec_type_t codec_type)
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t res;

  CSingleLock lock(m_Lock);

  if (m_viddec != -1)
    return true;

  // open decoder
  res = ismd_viddec_open(codec_type, &m_viddec);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "viddec open failed <%d>", res);
    return false;
  }

  res = ismd_viddec_get_input_port(m_viddec, &m_viddec_input_port);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "viddec get input port failed <%d>", res);
    return false;
  }

  res = ismd_viddec_get_output_port(m_viddec, &m_viddec_output_port);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "viddec get output port failed <%d>", res);
    return false;
  }

  res = ismd_viddec_set_pts_interpolation_policy(m_viddec,
      ISMD_VIDDEC_INTERPOLATE_MISSING_PTS, 0);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "ismd_viddec_set_pts_interpolation_policy failed <%d>",
        res);
    return false;
  }

  res = ismd_viddec_set_max_frames_to_decode(m_viddec, ISMD_VIDDEC_ALL_FRAMES);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "ismd_viddec_set_max_frames_to_decode failed <%d>",
        res);
    return false;
  }

  // set default fps
  /*
   res = ismd_viddec_set_frame_rate(m_viddec, 30000, 1001, false);
   if (res != ISMD_SUCCESS)
   {
   CLog::Log(LOGERROR, "ismd_viddec_set_frame_rate failed <%d>", res);
   return false;
   }
   */

  /*
   res = ismd_viddec_set_frame_output_policy(m_viddec, ISMD_VIDDEC_DECODE_ORDER);
   if (res != ISMD_SUCCESS)
   {
   CLog::Log(LOGERROR, "ismd_viddec_set_frame_output_policy failed <%d>", res);
   return false;
   }
   */
  SetVideoDecoderState(ISMD_DEV_STATE_PAUSE);

  m_video_codec = codec_type;

  return true;
}

bool CIntelSMDGlobals::CreateVidDecUserDataPort()
{
  ismd_result_t res;

  res = ismd_viddec_get_user_data_port(m_viddec, &m_viddec_user_data_port);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::CreateVidDecUserDataPort ismd_viddec_get_user_data_port failed <%d>",
        res);
    return false;
  }

  res = ismd_event_alloc(&m_viddec_user_data_event);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::CreateVidDecUserDataPort ismd_event_alloc failed <%d>",
        res);
    return false;
  }

  res = ismd_port_attach(m_viddec_user_data_port, m_viddec_user_data_event,
      ISMD_QUEUE_EVENT_NOT_EMPTY, ISMD_QUEUE_WATERMARK_NONE);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::CreateVidDecUserDataPort ismd_port_attach failed <%d>",
        res);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::DeleteVideoDecoder()
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t res;

  if (m_viddec == -1)
    return true;

  SetVideoDecoderState(ISMD_DEV_STATE_STOP);
  FlushVideoDecoder();

  ismd_dev_close(m_viddec);

  m_viddec = -1;
  m_viddec_input_port = -1;
  m_viddec_output_port = -1;
  m_viddec_user_data_port = -1;
  m_viddec_user_data_event = -1;
  m_bDemuxToVideo = false;

  return true;
}

bool CIntelSMDGlobals::PrintVideoStreamStats()
{
  ismd_result_t result;
  ismd_viddec_stream_statistics_t stat;

  result = ismd_viddec_get_stream_statistics(m_viddec, &stat);

  if (result == ISMD_SUCCESS)
    CLog::Log(LOGDEBUG,
        "current_bit_rate %lu \
        total_bytes %lu \
        frames_decoded %lu \
        frames_dropped %lu \
        error_frames %lu \
        invalid_bitstream_syntax_errors %lu \
        unsupported_profile_errors %lu \
        unsupported_level_errors %lu \
        unsupported_feature_errors %lu \
        unsupported_resolution_errors %lu ",

        stat.current_bit_rate, stat.total_bytes, stat.frames_decoded,
        stat.frames_dropped, stat.error_frames,
        stat.invalid_bitstream_syntax_errors, stat.unsupported_profile_errors,
        stat.unsupported_level_errors, stat.unsupported_feature_errors,
        stat.unsupported_resolution_errors);
  else
    CLog::Log(LOGERROR, "CIntelSMDGlobals::PrintVideoStreamStats failed");

  return (result == ISMD_SUCCESS);
}

bool CIntelSMDGlobals::PrintVideoStreaProp()
{
  ismd_result_t result;

  ismd_viddec_stream_properties_t prop;

  result = ismd_viddec_get_stream_properties(m_viddec, &prop);

  if (result == ISMD_SUCCESS)
    CLog::Log(LOGDEBUG,
        "codec_type %lu \
        nom_bit_rate %lu \
        frame_rate_num %lu \
        frame_rate_den %lu \
        coded_height %lu \
        coded_width %lu \
        display_height %lu \
        display_width %lu \
        profile %lu \
        level %lu \
        is_stream_interlaced %lu \
        sample_aspect_ratio %lu ",
        prop.codec_type, prop.nom_bit_rate, prop.frame_rate_num,
        prop.frame_rate_den, prop.coded_height, prop.coded_width,
        prop.display_height, prop.display_width, prop.profile, prop.level,
        prop.is_stream_interlaced, prop.sample_aspect_ratio);
  else
    CLog::Log(LOGERROR, "CIntelSMDGlobals::PrintVideoStreaProp failed");

  return (result == ISMD_SUCCESS);
}

bool CIntelSMDGlobals::CreateVideoRender(gdl_plane_id_t plane)
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t res;

  CSingleLock lock(m_Lock);

  if (m_video_proc == -1)
  {
    //Open and initialize DPE
    res = ismd_vidpproc_open(&m_video_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_open failed. %d", res);
      return false;
    }

    // We use SCALE_TO_FIT here for the scaling policy because it is easier for
    // us to create the dest rect based on baserenderer and tell SMD to scale that way,
    // than to reconcile the aspect ratio calculations and convoluded scaling policies
    // in SMD which appear to have many edge cases where it 'gives up'
    res = ismd_vidpproc_set_scaling_policy(m_video_proc, SCALE_TO_FIT);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_set_scaling_policy failed. %d", res);
      return false;
    }

    res = ismd_vidpproc_get_input_port(m_video_proc, &m_video_input_port_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_get_input_port failed. %d", res);
      return false;
    }

    res = ismd_vidpproc_get_output_port(m_video_proc,
        &m_video_output_port_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_get_output_port failed. %d", res);
      return false;
    }

    res = ismd_vidpproc_pan_scan_disable(m_video_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_pan_scan_disable failed. %d", res);
      return false;
    }

    res = ismd_vidpproc_gaussian_enable(m_video_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_gaussian_enable failed. %d", res);
      return false;
    }

    res = ismd_vidpproc_deringing_enable(m_video_proc);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_vidpproc_deringing_enable failed. %d", res);
      return false;
    }
  }

  if (m_video_render == -1)
  {
    //Open and initialize Render
    res = ismd_vidrend_open(&m_video_render);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ERROR: FAIL TO OPEN VIDEO RENDERER .....");
      return false;
    }

    res = ismd_vidrend_get_input_port(m_video_render,
        &m_video_input_port_renderer);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ERROR: FAIL TO GET INPUT PORT FOR RENDERER .....");
      return false;
    }

    res = ismd_dev_set_clock(m_video_render, m_main_clock);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ERROR: FAIL TO SET CLOCK FOR RENDERER .....");
      return false;
    }

    res = ismd_clock_set_vsync_pipe(m_main_clock, 0);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ERROR: ismd_clock_set_vsync_pipe .....");
      return false;
    }

    res = ismd_vidrend_set_video_plane(m_video_render, plane);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, " ERROR: FAIL TO SET GDL UPP FOR THIS RENDERER .....");
      return false;
    }

    res = ismd_vidrend_set_flush_policy(m_video_render,
        ISMD_VIDREND_FLUSH_POLICY_REPEAT_FRAME);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::CreateVideoRender ismd_vidrend_set_flush_policy failed");
      return false;
    }

    res = ismd_vidrend_set_stop_policy(m_video_render,
        ISMD_VIDREND_STOP_POLICY_DISPLAY_BLACK);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::CreateVideoRender ismd_vidrend_set_stop_policy failed");
      return false;
    }

    res = ismd_vidrend_enable_max_hold_time(m_video_render, 30, 1);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::CreateVideoRender ismd_vidrend_enable_max_hold_time failed");
      return false;
    }
  }

  //Connect DPE and Render
  if (m_video_output_port_proc != -1 && m_video_input_port_renderer != -1)
  {
    res = ismd_port_connect(m_video_output_port_proc,
        m_video_input_port_renderer);
    if (res != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "IntelSMDGlobals::CreateVideoRender ismd_port_connect failed");
      return false;
    }
  }
  else
  {
    CLog::Log(LOGERROR, "IntelSMDGlobals::CreateVideoRender failed");
    return false;
  }

  SetVideoRenderState(ISMD_DEV_STATE_PAUSE);
  m_RenderState = ISMD_DEV_STATE_PAUSE;

  return true;
}

bool CIntelSMDGlobals::MuteVideoRender(bool mute)
{
  ismd_result_t res;

  if (m_video_render == -1)
      return false;

  res = ismd_vidrend_mute(m_video_render, mute ? ISMD_VIDREND_MUTE_DISPLAY_BLACK_FRAME : ISMD_VIDREND_MUTE_NONE);
  if (res != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::MuteVideoRender with %d failed. %d", mute, res);
    return false;
  }

  return true;
}

float CIntelSMDGlobals::GetRenderFPS()
{
  ismd_result_t res;
  float fps = 0;
  ismd_vidrend_status_t status;
  ismd_vidrend_stats_t stat;

  if (m_video_render == -1)
    return 0;

  res = ismd_vidrend_get_status(m_video_render, &status);
  if (res == ISMD_SUCCESS)
    res = ismd_vidrend_get_stats(m_video_render, &stat);

  if (res == ISMD_SUCCESS)
    // we want to make sure some frames were actually rendered
    if (stat.frames_displayed > 30)
      fps = 90000.0f / status.content_pts_interval;

  return fps;
}

float CIntelSMDGlobals::GetDecoderFPS()
{
  float fps = 0;

  // check some frames were actually rendered
  if (GetRenderFPS() == 0)
    return 0;

  ismd_viddec_stream_properties_t prop;
  ismd_result_t res = ismd_viddec_get_stream_properties(GetVidDec(), &prop);
  if (res == ISMD_SUCCESS)
  {
    int num = prop.frame_rate_num;
    int den = prop.frame_rate_den;
    if (num != 0 && den != 0)
    {
      fps = (float) num / (float) den;
      //printf("SMD fps %f", fps);
    }
  }

  return fps;
}

ismd_result_t CIntelSMDGlobals::GetPortStatus(ismd_port_handle_t port,
    unsigned int& curDepth, unsigned int& maxDepth)
{
  ismd_result_t ret;
  ismd_port_status_t portStatus;

  ret = ismd_port_get_status(port, &portStatus);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::GetPortStatus ismd_port_get_status failed %d", ret);
    curDepth = -1;
    maxDepth = -1;
    return ret;
  }

  curDepth = portStatus.cur_depth;
  maxDepth = portStatus.max_depth;

  return ret;
}

double CIntelSMDGlobals::GetFrameDuration()
{
  ismd_result_t res;
  float duration = 0;
  ismd_vidrend_status_t status;
  ismd_vidrend_stats_t stat;

  if (m_video_render == -1)
    return 0;

  res = ismd_vidrend_get_status(m_video_render, &status);
  if (res == ISMD_SUCCESS)
    res = ismd_vidrend_get_stats(m_video_render, &stat);

  if (res == ISMD_SUCCESS)
    // we want to make sure some frames were actually rendered
    if (stat.frames_displayed > 30)
      duration = IsmdToDvdPts(status.content_pts_interval);

  return duration;
}

bool CIntelSMDGlobals::DeleteVideoRender()
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  SetVideoRenderState(ISMD_DEV_STATE_STOP);
  FlushVideoRender();

  if (m_video_proc != 1)
    ismd_dev_close(m_video_proc);
  if (m_video_render != -1)
    ismd_dev_close(m_video_render);

  m_video_proc = -1;
  m_video_input_port_proc = 0;
  m_video_output_port_proc = 0;

  m_video_render = -1;
  m_video_input_port_renderer = -1;
  m_video_output_port_renderer = -1;

  return true;
}

bool CIntelSMDGlobals::ConnectDecoderToRenderer()
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t ret;

  if (m_viddec_output_port != -1 && m_video_input_port_proc != -1)
  {
    ret = ismd_port_connect(m_viddec_output_port, m_video_input_port_proc);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "IntelSMDGlobals::ConnectDecoderToRenderer ismd_port_connect failed");
      return false;
    }
  }
  else
  {
    CLog::Log(LOGERROR, "IntelSMDGlobals::ConnectDecoderToRenderer failed");
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::PrintRenderStats()
{
  ismd_result_t result;
  ismd_vidrend_stats_t stat;

  result = ismd_vidrend_get_stats(m_video_render, &stat);

  if (result == ISMD_SUCCESS)
    CLog::Log(LOGDEBUG,
        "vsyncs_frame_received %d\
        vsyncs_frame_skipped %d\
        vsyncs_top_received %d\
        vsyncs_top_skipped %d\
        vsyncs_bottom_received %d\
        vsyncs_bottom_skipped %d\
        frames_input %d\
        frames_displayed %d\
        frames_released %d\
        frames_dropped %d\
        frames_repeated %d\
        frames_late %d\
        frames_out_of_order %d\
        frames_out_of_segment %d\
        late_flips %d",

        stat.vsyncs_frame_received, stat.vsyncs_frame_skipped,
        stat.vsyncs_top_received, stat.vsyncs_top_skipped,
        stat.vsyncs_bottom_received, stat.vsyncs_bottom_skipped,
        stat.frames_input, stat.frames_displayed, stat.frames_released,
        stat.frames_dropped, stat.frames_repeated, stat.frames_late,
        stat.frames_out_of_order, stat.frames_out_of_segment, stat.late_flips);
  else
    CLog::Log(LOGERROR, "CIntelSMDGlobals::PrintRenderStats failed");

  return result == ISMD_SUCCESS;
}

bool CIntelSMDGlobals::CreateAudioProcessor()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::CreateAudioProcessor");

  ismd_result_t result;

  if (m_audioProcessor != -1)
    return true;

  CHalServicesFactory::GetInstance().SetAudioDACState(false);

  result = ismd_audio_open_global_processor(&m_audioProcessor);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDAudioRenderer::Initialize - error open global processor: %d",
        result);
    CHalServicesFactory::GetInstance().SetAudioDACState(false);
    return false;
  }

  CHalServicesFactory::GetInstance().SetAudioDACState(true);

  return true;
}

bool CIntelSMDGlobals::DeleteAudioProcessor()
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t result;

  if (m_audioProcessor == -1)
    return true;

  CHalServicesFactory::GetInstance().SetAudioDACState(false);

  DeleteAudioDevice(true);
  DeleteAudioDevice(false);
  RemoveAudioOutput();

  result = ismd_audio_close_processor(m_audioProcessor);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::DestroyAudioProcessor - error closing device: %d",
        result);
    CHalServicesFactory::GetInstance().SetAudioDACState(true);
    return false;
  }

  m_audioProcessor = -1;

  // This should invalidate all devices
  m_audioDev = -1;
  m_audioDevNonTimed = -1;
  m_audio_inputPort = -1;
  m_audio_inputPortNonTimed = -1;
  for (int i = 0; i < 3; i++)
  {
    m_audio_output[i] = -1;
    m_audio_output_device_added[i] = false;
  }
  m_audio_output_count = -1;
  m_bDemuxToAudio = false;

  CHalServicesFactory::GetInstance().SetAudioDACState(true);

  return true;
}

bool CIntelSMDGlobals::CreateAudioDevice(bool timed)
{
  CLog::Log(LOGINFO, "CIntelSMDGlobals::CreateAudioDevice time %d", timed);

  ismd_result_t result;

  ismd_dev_t *audioDev = timed ? &m_audioDev : &m_audioDevNonTimed;
  ismd_port_handle_t *audioInputPort =
      timed ? &m_audio_inputPort : &m_audio_inputPortNonTimed;

  if (*audioDev == -1)
  {
    result = ismd_audio_add_input_port(m_audioProcessor, timed, audioDev,
        audioInputPort);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::CreateAudioDevice error add input port timed: %d",
          result);
      return false;
    }
  }

  if (m_main_clock == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateAudioDevice clock is -1");
    return false;
  }

  if (timed)
  {
    result = ismd_dev_set_clock(*audioDev, m_main_clock);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::CreateAudioDevice ismd_dev_set_clock failed");
      return false;
    }
  }

  return true;
}

bool CIntelSMDGlobals::DeleteAudioDevice(bool timed)
{
  CLog::Log(LOGDEBUG, "%s", __FUNCTION__);

  ismd_result_t result;

  ismd_dev_t audioDev = timed ? m_audioDev : m_audioDevNonTimed;

  if (audioDev == -1)
    return true;

  SetAudioDeviceState(ISMD_DEV_STATE_STOP, timed);
  FlushAudioDevice(timed);

  //    printf("ismd_dev_close");
  result = ismd_dev_close(audioDev);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::DeleteAudioDevice - error closing device: %d timed %d",
        result, timed);
    return false;
  }

  if (timed)
  {
    m_audioDev = -1;
    m_audio_inputPort = -1;
  }
  else
  {
    m_audioDevNonTimed = -1;
    m_audio_inputPortNonTimed = -1;
  }

  m_bDemuxToAudio = false;

  return true;
}

bool CIntelSMDGlobals::AddAudioOutput(int audio_device,
    ismd_audio_output_config_t device_config)
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::AddAudioOutput");

  ismd_result_t result;
  int hwdevice = -1;

  switch (audio_device)
  {
  case AUDIO_ANALOG:
    hwdevice = GEN3_HW_OUTPUT_I2S0;
    break;
  case AUDIO_DIGITAL_SPDIF:
    hwdevice = GEN3_HW_OUTPUT_SPDIF;
    break;
  case AUDIO_DIGITAL_HDMI:
    hwdevice = GEN3_HW_OUTPUT_HDMI;
    break;
  default:
    return false;
  }

  result = ismd_audio_add_phys_output(m_audioProcessor, hwdevice, device_config,
      &m_audio_output[audio_device]);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::AddAudioOutput - error add output: %d", result);
    return false;
  }

  m_audio_output_device_added[audio_device] = true;
  m_audio_output_count++;

  return true;
}

bool CIntelSMDGlobals::RemoveAudioOutput()
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::RemoveAudioOutput");

  ismd_result_t result;

  CHalServicesFactory::GetInstance().SetAudioDACState(false);

  for (int i = 0; i < sizeof(m_audio_output_device_added); i++)
  {
    if (m_audio_output_device_added[i])
    {
      result = ismd_audio_remove_output(m_audioProcessor, m_audio_output[i]);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR,
            "CIntelSMDGlobals::RemoveAudioOutput - error remove output: %d",
            result);
        CHalServicesFactory::GetInstance().SetAudioDACState(true);
        return false;
      }
      m_audio_output[i] = -1;
      m_audio_output_device_added[i] = false;
      m_audio_output_count--;
    }
  }

  CHalServicesFactory::GetInstance().SetAudioDACState(true);

  return true;
}

bool CIntelSMDGlobals::DisableAudioOutput()
{
  //printf("CIntelSMDGlobals::DisableAudioOutput");

  ismd_result_t result;

  if (m_audioProcessor == -1)
    return true;

  CHalServicesFactory::GetInstance().SetAudioDACState(false);

  for (int i = 0; i < sizeof(m_audio_output_device_added); i++)
  {
    if (m_audio_output_device_added[i])
    {
      result = ismd_audio_output_disable(m_audioProcessor, m_audio_output[i]);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR, "CIntelSMDGlobals::DisableAudioOutput failed - %d",
            result);
        CHalServicesFactory::GetInstance().SetAudioDACState(true);
        return false;
      }
    }
  }

  CHalServicesFactory::GetInstance().SetAudioDACState(true);

  return true;
}

bool CIntelSMDGlobals::EnableAudioOutput()
{
  //printf("CIntelSMDGlobals::EnableAudioOutput");

  ismd_result_t result;

  if (m_audioProcessor == -1)
    return true;

  CHalServicesFactory::GetInstance().SetAudioDACState(false);

  CLog::Log(LOGINFO, "CIntelSMDGlobals::EnableAudioOutput m_audio_output_device_added %d",
      m_audio_output_count + 1);

  for (int i = 0; i < sizeof(m_audio_output_device_added); i++)
  {
    if (m_audio_output_device_added[i])
    {
      CLog::Log(LOGINFO, "Adding audio device %d", i);
      result = ismd_audio_output_enable(m_audioProcessor, m_audio_output[i]);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR,
            "CIntelSMDGlobals::EnableAudioOutput failed %d at %d", result, i);
        CHalServicesFactory::GetInstance().SetAudioDACState(true);
      }
    }
  }
  CHalServicesFactory::GetInstance().SetAudioDACState(true);

  return true;
}

bool CIntelSMDGlobals::EnableAudioInput(bool timed)
{
  ismd_result_t result;
  ismd_dev_t audioDev = timed ? m_audioDev : m_audioDevNonTimed;

  if (audioDev == -1 || m_audioProcessor == -1)
    return true;

  result = ismd_audio_input_enable(m_audioProcessor, audioDev);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::EnableAudioInput failed %d. Timed %d", result,
        timed);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::DisableAudioInput(bool timed)
{
  ismd_result_t result;
  ismd_dev_t audioDev = timed ? m_audioDev : m_audioDevNonTimed;

  if (audioDev == -1 || m_audioProcessor == -1)
    return true;

  result = ismd_audio_input_disable(m_audioProcessor, audioDev);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::DisableAudioInput failed %d. Timed %d", result,
        timed);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::SetAudioDeviceState(ismd_dev_state_t state, bool timed)
{
  ismd_dev_t device = timed ? m_audioDev : m_audioDevNonTimed;

  if (state == ISMD_DEV_STATE_INVALID)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SetAudioDeviceState setting device to invalid");
    return true;
  }

  CSingleLock lock(m_Lock);

  ismd_result_t ret;

  if (device == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::SetAudioDeviceState device = -1");
    return false;
  }

  ret = ismd_dev_set_state(device, state);
  if (ret != ISMD_SUCCESS)
    CLog::Log(
        LOGWARNING,
        "CIntelSMDGlobals::SetAudioState ismd_dev_set_state %d on audio device timed %d failed %d",
        state, timed, ret);

  return true;
}

ismd_dev_state_t CIntelSMDGlobals::GetAudioDeviceState(bool timed)
{
  //printf("CIntelSMDGlobals::GetAudioDeviceState");

  CSingleLock lock(m_Lock);

  ismd_result_t ret;
  ismd_dev_state_t state;

  ismd_dev_t audioDev = timed ? m_audioDev : m_audioDevNonTimed;

  if (audioDev == -1)
  {
    //printf("CIntelSMDGlobals::GetAudioDeviceState audioDev = -1");
    return ISMD_DEV_STATE_INVALID;
  }

  ret = ismd_dev_get_state(audioDev, &state);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::GetAudioDeviceState ismd_dev_get_state failed %d",
        ret);
    return ISMD_DEV_STATE_INVALID;
  }

  return state;
}

bool CIntelSMDGlobals::SetVideoDecoderState(ismd_dev_state_t state)
{
  //printf("%s Video Decoder State: %d", __FUNCTION__, state);
  CSingleLock lock(m_Lock);

  ismd_result_t ret;

  if (state == ISMD_DEV_STATE_INVALID)
  {
    CLog::Log(LOGWARNING,
        "CIntelSMDGlobals::SetVideoDecoderState setting device to invalid");
    return true;
  }

  if (m_viddec != -1)
  {
    ret = ismd_dev_set_state(m_viddec, state);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(
          LOGWARNING,
          "CIntelSMDGlobals::SetVideoDecoderState ismd_dev_set_state on video decoder failed %d",
          ret);
      return true;
    }
  }

  CLog::Log(LOGDEBUG, "SetVideoDecoderState state %d ret %d", state, ret);

  return true;
}

bool CIntelSMDGlobals::SetVideoRenderState(ismd_dev_state_t state)
{
  //printf("%s Video Render State: %d", __FUNCTION__, state);

  CSingleLock lock(m_Lock);

  ismd_result_t ret;

  if (state == ISMD_DEV_STATE_INVALID)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::SetVideoRenderState setting device to invalid");
    return true;
  }

  if (m_video_proc != -1)
  {
    ret = ismd_dev_set_state(m_video_proc, state);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::SetVideoRenderState ismd_dev_set_state on video proc failed %d",
          ret);
      return false;
    }
  }

  /*
   if(state == ISMD_DEV_STATE_PLAY)
   {
   ret = ismd_vidrend_set_flush_policy(m_video_render, ISMD_VIDREND_FLUSH_POLICY_DISPLAY_BLACK);
   if (ret != ISMD_SUCCESS)
   {
   printf("CIntelSMDGlobals::SetVideoRenderState ismd_vidrend_set_flush_policy failed before play");
   return false;
   }
   }
   */

  if (m_video_render != -1)
  {
    ret = ismd_dev_set_state(m_video_render, state);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::SetVideoRenderState ismd_dev_set_state on video render failed %d",
          ret);
      return false;
    }
  }

  /*
   if(state == ISMD_DEV_STATE_PAUSE)
   {
   ret = ismd_vidrend_set_flush_policy(m_video_render, ISMD_VIDREND_FLUSH_POLICY_REPEAT_FRAME);
   if (ret != ISMD_SUCCESS)
   {
   printf("CIntelSMDGlobals::SetVideoRenderState ismd_vidrend_set_flush_policy failed after pause");
   return false;
   }
   }
   */

  m_RenderState = state;

  CLog::Log(LOGDEBUG, "SetRenderrState state %d ret %d", state, ret);

  return true;
}

bool CIntelSMDGlobals::FlushAudioDevice(bool timed /* = true */)
{
  //printf("CIntelSMDGlobals::FlushAudioDevice", __FUNCTION__);

  CSingleLock lock(m_Lock);
  ismd_result_t ret;

  ismd_dev_t audioDev = timed ? m_audioDev : m_audioDevNonTimed;

  SetAudioDeviceState(ISMD_DEV_STATE_PAUSE, timed);

  if (audioDev != -1)
  {
    ret = ismd_dev_flush(audioDev);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGDEBUG,
          "CIntelSMDGlobals::FlushAudioDevice ismd_dev_flush timed failed %d",
          ret);
      return false;
    }
  }

  return true;
}

bool CIntelSMDGlobals::FlushVideoDecoder()
{
  CSingleLock lock(m_Lock);
  ismd_result_t ret;

  if(m_viddec == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::FlushVideoDecoder m_viddec == -1");
    return false;
  }

  SetVideoDecoderState(ISMD_DEV_STATE_PAUSE);

  if (m_viddec != -1)
    ret = ismd_dev_flush(m_viddec);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::FlushVideoDecoder ismd_dev_flush failed %d",
        ret);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::FlushVideoRender()
{
  //printf("CIntelSMDGlobals::FlushVideoRender");

  CSingleLock lock(m_Lock);
  ismd_result_t ret;

  if(m_video_render == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::FlushVideoDecoder m_video_render == -1");
    return false;
  }

  if (m_video_render != -1)
    ret = ismd_vidrend_set_flush_policy(m_video_render,
        ISMD_VIDREND_FLUSH_POLICY_REPEAT_FRAME);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::CreateVideoRender ismd_vidrend_set_flush_policy failed after pause");
    return false;
  }

  SetVideoRenderState(ISMD_DEV_STATE_PAUSE);

  if (m_video_proc != -1)
    ret = ismd_dev_flush(m_video_proc);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::FlushVideoRender ismd_dev_flush video proc failed %d",
        ret);
    return false;
  }

  if (m_video_render != -1)
    ret = ismd_dev_flush(m_video_render);
  if (ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::FlushVideoRender ismd_dev_flush failed %d",
        ret);
    return false;
  }

  return true;
}

bool CIntelSMDGlobals::SetVideoRenderBaseTime(ismd_time_t time)
{
  //printf("%s", __FUNCTION__);
  CLog::Log(LOGINFO, "SetVideoRenderBaseTime base time %.2f",
      IsmdToDvdPts(time) / 1000000);

  ismd_result_t ret;
  ismd_dev_state_t state = ISMD_DEV_STATE_INVALID;

  CSingleLock lock(m_Lock);

  if (m_RenderState == ISMD_DEV_STATE_PLAY)
  {
    //printf("Warning - Trying to set base time on renderer while in play mode. Ignoring");
    return true;
  }

  if (m_video_render != -1)
  {
//    printf("CIntelSMDGlobals::SetVideoRenderBaseTime current state %d setting time to %.2f", state, IsmdToDvdPts(time) / 1000000.0f);
    ret = ismd_dev_get_state(m_video_render, &state);
    if (ret != ISMD_SUCCESS)
    {
      // Currently SDK always return error when trying to get renderer state
      //printf("CIntelSMDGlobals::SetVideoRenderBaseTime ismd_dev_get_state failed %d", ret);
    }
    else if (state == ISMD_DEV_STATE_PLAY)
    {
      CLog::Log(LOGDEBUG, "CIntelSMDGlobals::SetVideoRenderBaseTime device is running/n");
      return true;
    }

    //printf("CIntelSMDGlobals::SetVideoRenderBaseTime current state %d setting time to %.2f", state, IsmdToDvdPts(time) / 1000000.0f);

    ret = ismd_dev_set_stream_base_time(m_video_render, time);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::SetVideoRenderBaseTime ismd_dev_set_stream_base_time for video render failed %d",
          ret);
      return false;
    }
  }

  return true;
}

bool CIntelSMDGlobals::SetAudioDeviceBaseTime(ismd_time_t time)
{
  //printf("%s", __FUNCTION__);
  CLog::Log(LOGINFO, "SetAudioDeviceBaseTime base time %.2f",
      IsmdToDvdPts(time) / 1000000);

  ismd_result_t ret;
  ismd_dev_state_t state = ISMD_DEV_STATE_INVALID;

  CSingleLock lock(m_Lock);

  if (m_audioDev != -1)
  {
//    printf("CIntelSMDGlobals::SetAudioDeviceBaseTime current state %d setting time to %.2f", state, IsmdToDvdPts(time) / 1000000.0f);
    ismd_dev_get_state(m_audioDev, &state);
    if (state == ISMD_DEV_STATE_PLAY)
    {
      CLog::Log(LOGDEBUG,
          "CIntelSMDGlobals::SetAudioDeviceBaseTime device is running. Ignoring request");
      return true;
    }

    //printf("CIntelSMDGlobals::SetAudioDeviceBaseTime current state %d setting time to %.2f", state, IsmdToDvdPts(time) / 1000000.0f);

    ret = ismd_dev_set_stream_base_time(m_audioDev, time);
    if (ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDGlobals::SetAudioDeviceState ismd_dev_set_stream_base_time for audio device failed");
    }
  }

  return true;
}

void CIntelSMDGlobals::Mute(bool bMute)
{
  ismd_audio_processor_t audioProcessor = g_IntelSMDGlobals.GetAudioProcessor();
  if (audioProcessor == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::%s - audioProcessor == -1",
        __FUNCTION__);
    return;
  }
  ismd_audio_mute(audioProcessor, bMute);
}

bool CIntelSMDGlobals::SetMasterVolume(int nVolume)
{
  // we disable volume control with the box

  ismd_audio_processor_t audioProcessor = g_IntelSMDGlobals.GetAudioProcessor();
  if (audioProcessor == -1)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::SetMasterVolume - audioProcessor == -1");
    return false;
  }

  bool muted = false;
  ismd_result_t result = ismd_audio_is_muted(audioProcessor, &muted);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR,
        "CIntelSMDGlobals::CIntelSMDGlobals - ismd_audio_is_muted failed.  %d",
        result);
    return false;
  }

  if (nVolume == VOLUME_MINIMUM)
  {
    if (!muted)
      result = ismd_audio_mute(audioProcessor, true);
  }
  else
  {
    if (muted)
      result = ismd_audio_mute(audioProcessor, false);

    if (result == ISMD_SUCCESS)
    {
      // SMD volume scales from +18.0dB to -145.0dB with values of +180 to -1450
      // anything over 0dB will cause audio degradation
      // in practice -50dB is extremely quiet, so we scale our standard volume from MAX_VOLUME to -30dB
      long smdVolume = (long) (((float) nVolume) * 300.0f / 6000.0f);
      result = ismd_audio_set_master_volume(audioProcessor, smdVolume);
    }

    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,
          "CIntelSMDAudioRenderer::Resume - ismd_audio_set_master_volume: %d",
          result);
    }
  }

  return (result == ISMD_SUCCESS);
}

bool CIntelSMDGlobals::CheckCodecHDMIBitstream(int Codec)
{
  for (int i = 0; i < 6; i++)
  {
    if (m_HDMIBitstreamCodecs[i][0] == Codec)
      return (bool) m_HDMIBitstreamCodecs[i][1];
  }
  return false;
}
bool CIntelSMDGlobals::CheckCodecHWDecode(int Codec)
{
  for (int i = 0; i < HW_AUDIO_DECODING_NUM; i++)
  {
    if (m_HWDecodeCodecs[i][0] == Codec)
      return (bool) m_HWDecodeCodecs[i][1];
  }
  return false;
}

void CIntelSMDGlobals::SetCodecHDMIBitstream(int Codec, bool val)
{
  int i;
  for (i = 0; i < 6 && m_HDMIBitstreamCodecs[i][0]; i++)
  {
    if (m_HDMIBitstreamCodecs[i][0] == Codec)
      break;
  }
  if (i != 6)
  {
    m_HDMIBitstreamCodecs[i][0] = Codec;
    m_HDMIBitstreamCodecs[i][1] = (int) val;
  }
  else
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::SetCodecHDMIBitstream -- too many codecs (this one is %d",
        Codec);

}

void CIntelSMDGlobals::SetCodecHWDecode(int Codec)
{
  CLog::Log(LOGDEBUG, "CIntelSMDGlobals::SetCodecHWDecode codec %d", Codec);

  int i;
  for (i = 0; i < HW_AUDIO_DECODING_NUM && m_HWDecodeCodecs[i][0]; i++)
  {
    if (m_HWDecodeCodecs[i][0] == Codec)
      break;
  }
  if (i != HW_AUDIO_DECODING_NUM)
  {
    m_HWDecodeCodecs[i][0] = Codec;
    m_HWDecodeCodecs[i][1] = (int) true;
  }
  else
    CLog::Log(
        LOGERROR,
        "CIntelSMDGlobals::SetCodecHWDecode -- too many codecs (this one is %d",
        Codec);
}

ismd_dev_state_t CIntelSMDGlobals::DVDSpeedToSMD(int dvdSpeed)
{
  switch (dvdSpeed)
  {
  case DVD_PLAYSPEED_PAUSE:
    return ISMD_DEV_STATE_PAUSE;
  case DVD_PLAYSPEED_NORMAL:
    return ISMD_DEV_STATE_PLAY;
  case DVD_PLAYSPEED_STOP:
    return ISMD_DEV_STATE_STOP;
  default:
    return ISMD_DEV_STATE_PLAY;
  }
}

#endif