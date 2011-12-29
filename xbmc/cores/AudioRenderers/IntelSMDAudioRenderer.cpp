#include "system.h"
#include "Settings.h"
#include "GUISettings.h"
#include "IntelSMDAudioRenderer.h"
#include <cmath>

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

#include "IntelSMDGlobals.h"
#include "AudioContext.h"
#include "cores/dvdplayer/DVDClock.h"
#include "FileSystem/SpecialProtocol.h"
#include "utils/SingleLock.h"
#include "utils/log.h"
#include "AdvancedSettings.h"
#include "Application.h"

#include "LicenseConfig.h"

extern "C"
{
#include <ismd_core.h>
#include <ismd_audio.h>
#include <ismd_audio_ac3.h>
#include <ismd_audio_ddplus.h>
#include <ismd_audio_truehd.h>
#include <ismd_audio_dts.h>
#include <pal_soc_info.h>
}
#ifndef UINT64_C
#define UINT64_C(x) (const unsigned long long)(x)
#endif

extern "C" {
#if (defined USE_EXTERNAL_FFMPEG)
  #if (defined HAVE_LIBAVCODEC_AVCODEC_H)
    #include <libavcodec/avcodec.h>
  #elif (defined HAVE_FFMPEG_AVCODEC_H)
    #include <ffmpeg/avcodec.h>
  #endif
#else
  #include "ffmpeg/libavcodec/avcodec.h"
#endif
}


#define NOT_TIMED 0
#define TIMED 1

#define AUDIO_OUTPUT_DELAY 39 // requested by dolby cert

using namespace std;

CIntelSMDAudioRenderer* CIntelSMDAudioRenderer::m_owner = NULL;
CCriticalSection CIntelSMDAudioRenderer::m_SMDAudioLock;
unsigned int CIntelSMDAudioRenderer::m_uiBytesPerSecond = -1;
int CIntelSMDAudioRenderer::m_inputChannelConfig = -1;
ismd_audio_channel_config_t CIntelSMDAudioRenderer::m_outputChannelConfig = ISMD_AUDIO_CHAN_CONFIG_INVALID;
int CIntelSMDAudioRenderer::m_outputChannelMap = -1;
ismd_audio_output_mode_t CIntelSMDAudioRenderer::m_outputOutMode = ISMD_AUDIO_OUTPUT_INVALID;
unsigned int CIntelSMDAudioRenderer::m_outputSampleRate = 0;
ismd_audio_format_t CIntelSMDAudioRenderer::m_inputAudioFormat = ISMD_AUDIO_MEDIA_FMT_INVALID;
int CIntelSMDAudioRenderer::m_ioBitsPerSample = -1;
int CIntelSMDAudioRenderer::m_inputSamplesPerSec = -1;
bool CIntelSMDAudioRenderer::m_bAllOutputs = false;
bool CIntelSMDAudioRenderer::m_bIsInit = false;
CIntelSMDAudioRenderer::edidHint* CIntelSMDAudioRenderer::m_edidTable = NULL;
ismd_audio_downmix_mode_t CIntelSMDAudioRenderer::m_DownmixMode = ISMD_AUDIO_DOWNMIX_DEFAULT;
int CIntelSMDAudioRenderer::m_nSampleRate = 48000;

static const unsigned int s_edidRates[] = {32000,44100,48000,88200,96000,176400,192000};
static const unsigned int s_edidSampleSizes[] = {16,20,24,32};

union pts_union
{
  double  pts_d;
  int64_t pts_i;
};

static int64_t pts_dtoi(double pts)
{
  pts_union u;
  u.pts_d = pts;
  return u.pts_i;
}

static double pts_itod(int64_t pts)
{
  pts_union u;
  u.pts_i = pts;
  return u.pts_d;
}

void ModifyChannelsByLicense(int& iChannels, VENDOR vendor)
{
    if((g_lic_settings.get_max_channels_decode(vendor)>-1) && (iChannels > g_lic_settings.get_max_channels_decode(vendor)))
    {
        iChannels = g_lic_settings.get_max_channels_decode(vendor);
    }
}

void dump_audio_licnense_file()
{
  CLog::Log(LOGINFO, "Audio Licenses:\n");
  CLog::Log(LOGINFO, "get_max_channels_decode dolby %d\n", g_lic_settings.get_max_channels_decode(AUDIO_VENDOR_DOLBY));
  CLog::Log(LOGINFO, "get_max_channels_encode dolby %d\n", g_lic_settings.get_max_channels_encode(AUDIO_VENDOR_DOLBY));
  CLog::Log(LOGINFO, "get_max_channels_decode DTS %d\n", g_lic_settings.get_max_channels_decode(AUDIO_VENDOR_DTS));
  CLog::Log(LOGINFO, "get_max_channels_encode DTS %d\n", g_lic_settings.get_max_channels_encode(AUDIO_VENDOR_DTS));

  CLog::Log(LOGINFO, "is_dolby_sw_decode_allowed %d\n", g_lic_settings.is_dolby_sw_decode_allowed());
  CLog::Log(LOGINFO, "is_dts_sw_decode_allowed %d\n", g_lic_settings.is_dts_sw_decode_allowed());
  CLog::Log(LOGINFO, "get_preferred_encoding ");
  if(g_lic_settings.get_preferred_encoding() == AUDIO_VENDOR_DOLBY) CLog::Log(LOGINFO, "Dolby\n");
  else if(g_lic_settings.get_preferred_encoding() == AUDIO_VENDOR_DTS) CLog::Log(LOGINFO, "DTS\n");
  else CLog::Log(LOGINFO, "NONE\n");
}

CIntelSMDAudioRenderer::CIntelSMDAudioRenderer()
{
  m_bIsAllocated = false;
  m_bFlushFlag = true;
  m_dwChunkSize = 0;
  m_dwBufferLen = 0;
  m_lastPts = ISMD_NO_PTS;
  m_lastSync = ISMD_NO_PTS;
  
  m_channelMap = NULL;
  m_uiBitsPerSample = 0;
  m_uiChannels = 0;
  m_audioMediaFormat = AUDIO_MEDIA_FMT_PCM;
  m_uiSamplesPerSec = 0;
  m_bTimed = false;
  m_bPassthrough = false;
  m_bDisablePtsCorrection = false;
  m_pChunksCollected = NULL;
  m_bIsUIAudio = false;
}

bool
CIntelSMDAudioRenderer::Initialize(IAudioCallback* pCallback, 
                                   int iChannels,
                                   enum PCMChannels* channelMap,
                                   unsigned int uiSamplesPerSec,
                                   unsigned int uiBitsPerSample,
                                   bool bResample,
                                   const char* strAudioCodec,
                                   bool bIsMusic, 
                                   bool bPassthrough, 
                                   bool bTimed, 
                                   AudioMediaFormat audioMediaFormat)
{
  CLog::Log(LOGINFO, "CIntelSMDAudioRenderer Initialize iChannels %d uiSamplesPerSec %d uiBitsPerSample %d audioMediaFormat %d bPassthrough %d bTimed %d\n",
      iChannels, uiSamplesPerSec, uiBitsPerSample, audioMediaFormat, bPassthrough, bTimed);

  CSingleLock lock(m_SMDAudioLock);

  g_lic_settings.Load();
  dump_audio_licnense_file();

  ismd_audio_processor_t  audioProcessor = -1;
  ismd_dev_t              audioDev = -1;
  ismd_port_handle_t      audio_inputPort = -1;
  int                     audio_output_device = -1;
  ismd_audio_format_t     ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_INVALID;
  VENDOR decoding_vendor = AUDIO_VENDOR_NONE;

  bool initOutput = false;
  bool bAllowPoorAccuracy = true;
  bool bUsePassthrough = bPassthrough;
  bool bAC3Encode = false;
  int audioOutputMode = g_guiSettings.GetInt("audiooutput.mode");
  static bool bCanDDEncode = false;
  static bool bCanTrueHDBitstream = false;


  if (!m_bIsInit)
  {
    initOutput = true;

    // Determine our hw decode support
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_DD))
      g_IntelSMDGlobals.SetCodecHWDecode(CODEC_ID_AC3);
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_DD_PLUS))
      g_IntelSMDGlobals.SetCodecHWDecode(CODEC_ID_EAC3);
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_TRUE_HD))
      g_IntelSMDGlobals.SetCodecHWDecode(CODEC_ID_TRUEHD);
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_DTS))
      g_IntelSMDGlobals.SetCodecHWDecode(CODEC_ID_DTS);
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_DTS_HD))
      g_IntelSMDGlobals.SetCodecHWDecode(SMD_CODEC_DTSHD);
    if (ISMD_SUCCESS == ismd_audio_codec_available(ISMD_AUDIO_MEDIA_FMT_AAC_LOAS))
      g_IntelSMDGlobals.SetCodecHWDecode(CODEC_ID_AAC_LATM);

    /*
     if( ISMD_SUCCESS == ismd_audio_codec_available( ISMD_AUDIO_MEDIA_FMT_MPEG ) )
     g_IntelSMDGlobals.SetCodecHWDecode( CODEC_ID_MP3 );
     if( ISMD_SUCCESS == ismd_audio_codec_available( ISMD_AUDIO_MEDIA_FMT_AAC ) )
     g_IntelSMDGlobals.SetCodecHWDecode( CODEC_ID_AAC );
     */

    // Determine our hw encode support
    if (ISMD_SUCCESS == ismd_audio_codec_available((ismd_audio_format_t) ISMD_AUDIO_ENCODE_FMT_AC3))
      bCanDDEncode = true;

    if (ISMD_SUCCESS == ismd_audio_codec_available((ismd_audio_format_t) ISMD_AUDIO_ENCODE_FMT_TRUEHD_MAT))
      bCanTrueHDBitstream = true;
  }

  // Map the media format to ISMD_
  switch (audioMediaFormat)
  {
   case AUDIO_MEDIA_FMT_PCM:        ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_PCM;                                 break;
   case AUDIO_MEDIA_FMT_DVD_PCM:    ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DVD_PCM;                             break;
   case AUDIO_MEDIA_FMT_BLURAY_PCM: ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_BLURAY_PCM;                          break;
   case AUDIO_MEDIA_FMT_MPEG:       ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_MPEG;                                break;
   case AUDIO_MEDIA_FMT_AAC:        ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_AAC;                                 break;
   case AUDIO_MEDIA_FMT_AAC_LOAS:   ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_AAC_LOAS;                            break;
   case AUDIO_MEDIA_FMT_AAC_LATM:   ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_AAC_LOAS;                            break;
   case AUDIO_MEDIA_FMT_DD:         ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DD;         decoding_vendor = AUDIO_VENDOR_DOLBY; break;
   case AUDIO_MEDIA_FMT_DD_PLUS:    ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DD_PLUS;    decoding_vendor = AUDIO_VENDOR_DOLBY; break;
   case AUDIO_MEDIA_FMT_TRUE_HD:    ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_TRUE_HD;    decoding_vendor = AUDIO_VENDOR_DOLBY; break;
   case AUDIO_MEDIA_FMT_DTS_HD:     ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DTS_HD;     decoding_vendor = AUDIO_VENDOR_DTS;   break;
   case AUDIO_MEDIA_FMT_DTS_HD_HRA: ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DTS_HD_HRA; decoding_vendor = AUDIO_VENDOR_DTS;   break;
   case AUDIO_MEDIA_FMT_DTS_HD_MA:  ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DTS_HD_MA;  decoding_vendor = AUDIO_VENDOR_DTS;   break;
   case AUDIO_MEDIA_FMT_DTS:        ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DTS;        decoding_vendor = AUDIO_VENDOR_DTS;   break;
   case AUDIO_MEDIA_FMT_DTS_LBR:    ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_DTS_LBR;    decoding_vendor = AUDIO_VENDOR_DTS;   break;
   case AUDIO_MEDIA_FMT_WM9:        ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_WM9;                                 break;
   default:
     CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - unknown audio media format requested");
     return false;
  }

  m_bTimed = bTimed;

  g_IntelSMDGlobals.DeleteAudioProcessor();

  g_IntelSMDGlobals.CreateAudioProcessor();
  g_IntelSMDGlobals.CreateAudioDevice(m_bTimed);

  g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_STOP, m_bTimed);
  g_IntelSMDGlobals.FlushAudioDevice(m_bTimed);

  g_IntelSMDGlobals.DisableAudioInput(m_bTimed);

  // sets some defaults
  if(uiBitsPerSample == 0)
    uiBitsPerSample = 16;

  CStdString name(strAudioCodec);
  if(name.Equals("gui", false))
  {
    m_bIsUIAudio = true;
  }

  //
  // Input configuration
  //


  // Default buffer setup - we write in chan * 1024 aligned blocks for PCM data
  // otherwise we risk getting channels out of alignment
  m_dwBufferLen = 8 * 1024;
  if( AUDIO_MEDIA_FMT_PCM != audioMediaFormat )
  {
    m_dwChunkSize = 8 * 1024;
  }
  else
  {
    switch( iChannels )
    {
      case 0:
      case 1:
      case 2:
      case 4:
      case 8:
        m_dwChunkSize = 8 * 1024;
        break;
      case 3:
      case 5:
      case 6:
      case 7:
        m_dwChunkSize = iChannels * 1024;
        break;
    };
  }

  // Set up the channel configuration
  if (iChannels == 0)
    iChannels = 2;

  // we map 1 channel streaming audio (music) to stereo
  if(iChannels == 1 && !m_bTimed)
    uiSamplesPerSec /= 2;

  // Now allow specific codecs to adjust settings to ensure they behave properly with
  // ismd
  switch( audioMediaFormat )
  {
    case AUDIO_MEDIA_FMT_DD:
    {
      CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize DD detected\n");

      if( g_guiSettings.GetBool("audiooutput.ac3passthrough") &&
          (AUDIO_DIGITAL_HDMI == audioOutputMode || AUDIO_DIGITAL_SPDIF == audioOutputMode) &&
          uiSamplesPerSec == 48000 )
      {
        // this is passthrough
        m_dwChunkSize = 0;
        m_dwBufferLen = 8192;
      }
      else
      {
        // otherwise we are doing HW decode
        bUsePassthrough = false;
      }
      break;
    }
    case AUDIO_MEDIA_FMT_DD_PLUS:
    {
      CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize DD Plus detected\n");
      bool bEAC3pass = g_guiSettings.GetBool("audiooutput.eac3passthrough");
      bool bAC3pass = g_guiSettings.GetBool("audiooutput.ac3passthrough");

      //printf("CIntelSMDAudioRenderer::Initialize DD Plus detected. bEAC3pass %d bAC3pass %d uiSamplesPerSec %d iChannels %d\n",
      //    bEAC3pass, bAC3pass, uiSamplesPerSec, iChannels);

      if( bEAC3pass && AUDIO_DIGITAL_HDMI == audioOutputMode && uiSamplesPerSec == 48000 )
      {
        // this is passthrough
      }
      else if( bAC3pass &&
               bCanDDEncode && 
               (AUDIO_DIGITAL_HDMI == audioOutputMode || AUDIO_DIGITAL_SPDIF == audioOutputMode) &&
               uiSamplesPerSec == 48000)
      {
        // this is EAC3 -> AC3 re-encode
        bAC3Encode = true;
        if( iChannels > 6 )
          iChannels = 6;
      }
      else
      {
        // this is hw decode
        bUsePassthrough = false;
      }
      break;
    }
    case AUDIO_MEDIA_FMT_DTS:
    {
      CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize DTS detected\n");
      if( g_guiSettings.GetBool("audiooutput.dtspassthrough") &&
          (AUDIO_DIGITAL_HDMI == audioOutputMode || AUDIO_DIGITAL_SPDIF == audioOutputMode) &&
          uiSamplesPerSec == 48000 &&
          iChannels > 2)
      {
        // this is passthrough
        m_dwChunkSize = 0;
        m_dwBufferLen = 8192;
      }
      else
      {
        // this is hw decode
        bUsePassthrough = false;
        m_dwChunkSize = 0;
        m_dwBufferLen = 8192;
      }
      
      break;
    }
    case AUDIO_MEDIA_FMT_DTS_HD_MA:
    {
      // DTS-HD is marked as passthrough, packetized by the ismd libraries.
      CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize DTS-HD detected\n");
      
      m_dwBufferLen = 8 * 1024;
      m_dwChunkSize = 0;
      break;
    }
    case AUDIO_MEDIA_FMT_TRUE_HD:
    {
      CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize TrueHD detected\n");
      if( bUsePassthrough )
      {
        m_dwBufferLen = 8 * 1024;
        m_dwChunkSize = 0;
        if( !bCanTrueHDBitstream )
        {
          CLog::Log(LOGWARNING, "CIntelSMDAudioRenderer::Initialize missing MAT encoder; TrueHD bitstreaming may fail");
        }
      }
      break;
    }
    case AUDIO_MEDIA_FMT_AAC_LATM:
    case AUDIO_MEDIA_FMT_AAC_LOAS:
      break;
    case AUDIO_MEDIA_FMT_MPEG:
      break;
    case AUDIO_MEDIA_FMT_AAC:
      break;
    default:
    {
      // For all other formats treat as PCM
      if(!g_advancedSettings.m_bForceAudioHardwarePassthrough)
      {
        ismdAudioInputFormat = ISMD_AUDIO_MEDIA_FMT_PCM;
      }
      break;
    }
  }

  //We could use chunk 0 for all type of inputs, but this requires more testing
  if(audioMediaFormat == AUDIO_MEDIA_FMT_PCM && !m_bTimed)
    m_dwChunkSize = 0;

  CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize ismdAudioInputFormat = %d\n", ismdAudioInputFormat);

  // The following formats are the only one we use to output DDCO or DTS (when applicable)

  if ((audioMediaFormat == AUDIO_MEDIA_FMT_PCM) ||
      (audioMediaFormat == AUDIO_MEDIA_FMT_DVD_PCM) ||
      (audioMediaFormat == AUDIO_MEDIA_FMT_BLURAY_PCM) ||
      (audioMediaFormat == AUDIO_MEDIA_FMT_MPEG) ||
      (audioMediaFormat == AUDIO_MEDIA_FMT_AAC))
  { // For these inputs, use the preferred encoding, if applicable.
      switch(g_lic_settings.get_preferred_encoding())
      {
          case AUDIO_VENDOR_DOLBY:
          {
              bool bAC3pass = g_guiSettings.GetBool("audiooutput.ac3passthrough");
              if(bCanDDEncode && bAC3pass)
              {
                  if((g_lic_settings.get_max_channels_encode(AUDIO_VENDOR_DOLBY) >= iChannels) || ( -1 == g_lic_settings.get_max_channels_encode(AUDIO_VENDOR_DOLBY)))
                  {
                    CLog::Log(LOGINFO, "Setting AC3 encode by license\n");
                    bAC3Encode = true;
                  }
              }
              break;
          }
          case AUDIO_VENDOR_DTS:
          {
              CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize DTS preferred encoding is currently not supported by design\n");
              break;
          }
          default:
          {
              //Nothing to do here.
          }
      }
    }


  // This is only used when passing in multichannel PCM
  int inputChannelConfig = AUDIO_CHAN_CONFIG_INVALID;
  if( ISMD_AUDIO_MEDIA_FMT_PCM == ismdAudioInputFormat )
  {
    inputChannelConfig = BuildChannelConfig( channelMap, iChannels );
  }

  bool passthroughsetting = false;
  if (audioOutputMode == AUDIO_ANALOG)
    passthroughsetting = false;
  else if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DD)
    passthroughsetting = g_guiSettings.GetBool("audiooutput.ac3passthrough");
  else if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DD_PLUS)
    passthroughsetting = bAC3Encode || g_guiSettings.GetBool("audiooutput.eac3passthrough");
  else if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_TRUE_HD)
    passthroughsetting = g_guiSettings.GetBool("audiooutput.truehdpassthrough");
  else if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS || 
           ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS_LBR)
    passthroughsetting = g_guiSettings.GetBool("audiooutput.dtspassthrough");
  else if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS_HD || 
           ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS_HD_HRA ||
           ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS_HD_MA)
    passthroughsetting = g_guiSettings.GetBool("audiooutput.dtshdpassthrough");
  else
    passthroughsetting = false;

  if( bUsePassthrough && !passthroughsetting )
  {
    CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize bUsePassthrough is true but passthroughsetting is false; fixing\n");
    bUsePassthrough = passthroughsetting;
  }

  //
  // Output configuration
  //

  ismd_audio_output_config_t output_config;
  ismd_audio_input_pass_through_config_t passthrough_config;
  bool bAllOutputs = false;

  // Select device

  if (audioOutputMode == AUDIO_DIGITAL_HDMI)
    audio_output_device = GEN3_HW_OUTPUT_HDMI;
  else if (audioOutputMode == AUDIO_DIGITAL_SPDIF)
    audio_output_device = GEN3_HW_OUTPUT_SPDIF;
  else if (audioOutputMode == AUDIO_ANALOG)
    audio_output_device = GEN3_HW_OUTPUT_I2S0;
  else if (audioOutputMode == AUDIO_ALL_OUTPUTS)
  {
    // use analog for all compat checks and set alloutputs..
    audio_output_device = GEN3_HW_OUTPUT_I2S0;
    bAllOutputs = true;
  }
  
  if(audio_output_device == GEN3_HW_OUTPUT_I2S0)
  {
    bUsePassthrough = false;
    bAC3Encode = false;
  }


  if (bAC3Encode)
  {
    // Configure for decode and re-encode to DD
    CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize using DD encoder\n");
    output_config.out_mode = ISMD_AUDIO_OUTPUT_ENCODED_DOLBY_DIGITAL;
    passthrough_config.is_pass_through = false;
  }
  else if (bUsePassthrough)
  {
    // Configure for passthrough
    CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize using passthrough\n");
    output_config.out_mode = ISMD_AUDIO_OUTPUT_PASSTHROUGH;
    passthrough_config.is_pass_through = true;
    passthrough_config.dts_or_dolby_convert = false;
    passthrough_config.supported_format_count = 1;
    passthrough_config.supported_formats[0] = ismdAudioInputFormat;
  }
  else
  {
    // configure for decode
    if( ismdAudioInputFormat != ISMD_AUDIO_MEDIA_FMT_PCM )
      CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize using SMD audio decoder\n");
    else
      CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize not using passthrough\n");

    output_config.out_mode = ISMD_AUDIO_OUTPUT_PCM;
    passthrough_config.is_pass_through = false;
  }

  if (!bUsePassthrough)
  {
    ModifyChannelsByLicense(iChannels, decoding_vendor);
  }
  
  // Set the default out config
  output_config.sample_size = 16;
  output_config.stream_delay = 0;
  output_config.ch_map = 0;
  output_config.sample_rate = 48000;
  output_config.ch_config = ISMD_AUDIO_STEREO;

  // If we are using PCM out then set the channel configuration; in encoded passthrough it stays as stereo,
  // otherwise DD passthrough fails
  if( audio_output_device == GEN3_HW_OUTPUT_HDMI &&
       (!bUsePassthrough && g_guiSettings.GetBool("audiooutput.lpcm71passthrough")) )
  {
    if( iChannels == 8 )      output_config.ch_config = ISMD_AUDIO_7_1;
    else if( iChannels == 6 ) output_config.ch_config = ISMD_AUDIO_5_1;
    else                      output_config.ch_config = ISMD_AUDIO_STEREO;
  }

  if( GEN3_HW_OUTPUT_HDMI == audio_output_device )
  {
    if( !m_bIsInit ||
        !g_IntelSMDGlobals.GetAudioOutputAdded(AUDIO_DIGITAL_HDMI) )
    {
      if(g_guiSettings.GetBool("videoscreen.forceedid"))
      {
        UnloadEDID();
        (void)LoadEDID();
      }
      else // if not using edid, base receiver support on user settings
      {
        // Clear our bitstream support table
         CLog::Log(LOGNONE, "EDID is disabled. Settings all audio formats to true");
         g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_AC3,    true );
         g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_EAC3,   true );
         g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_TRUEHD, true );
         g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_DTS,    true );
         g_IntelSMDGlobals.SetCodecHDMIBitstream( SMD_CODEC_DTSHD, true );
      }
    }
  }

  // Last item - if we have audio data from EDID and are on HDMI, then validate the output
  unsigned int suggSampleRate = uiSamplesPerSec;
  unsigned int suggSampleSize = uiBitsPerSample;
  int suggChannels = iChannels;
  
  if( audio_output_device == GEN3_HW_OUTPUT_HDMI)
  {
    if(CheckEDIDSupport( bUsePassthrough ? ismdAudioInputFormat : ISMD_AUDIO_MEDIA_FMT_PCM,
        suggChannels, suggSampleRate, suggSampleSize ) )
    {
      output_config.sample_rate = suggSampleRate;
      output_config.sample_size = suggSampleSize;
    }
  }
  else if( audio_output_device == GEN3_HW_OUTPUT_SPDIF )
  {
    // For spdif, we support:
    // AC3 bitstream
    // DTS bitstream
    // PCM 32kHz-96Khz 16/24bit 2ch
    if( !bUsePassthrough )
    {
      if( iChannels > 2 )
      {
        CLog::Log(LOGERROR, "%s - SPDIF connections are limited to 2 channels for PCM", __FUNCTION__);
        output_config.ch_config = ISMD_AUDIO_STEREO;
      }
      if( uiBitsPerSample > 24 )
      {
        CLog::Log(LOGERROR, "%s - SPDIF connections are limited to 24 bits/sample", __FUNCTION__);
        output_config.sample_size = 24;
      }
      if( uiSamplesPerSec > 96000 )
      {
        CLog::Log(LOGERROR, "%s - SPDIF connections are limited to 96kHz sample rate", __FUNCTION__);
        output_config.sample_rate = 96000;
      }
    }
  }
  else
  {
    // Analog, or all outputs. In this case we need to force to 16bit/48khz
    output_config.sample_size = 16;
    output_config.stream_delay = 0;
    output_config.ch_map = 0;
    output_config.sample_rate = 48000;
  }

  // When using UI sound, always set the device to 48k
  // Otherwise we might get into invalid sampling conversions issues later (e.g 48k -> 44.1k)
  if(m_bIsUIAudio)
  {
    output_config.sample_rate = 48000;
  }

  // make sure sample rate is valid
  if(output_config.sample_rate == 24000)
    output_config.sample_rate = 48000;


  //
  // Log everything
  //
  CLog::Log(LOGINFO,
            "CIntelSMDAudioRenderer::Initialize - Channels: %i - SampleRate: %i - SampleBit: %i - "
            "Resample %s - Codec %s - IsMusic %s - IsPassthrough %s - AllOuts %s",
            iChannels, uiSamplesPerSec, uiBitsPerSample, bResample ? "true" : "false",
            strAudioCodec, bIsMusic ? "true" : "false", bUsePassthrough ? "true" : "false",
            bAllOutputs ? "true" : "false");



  //
  // All input and output settings are now set - see what hardware configuration is required
  //
  
  /****************************************************************************
   * Logic to determine what needs to be open/set. We want to minimize the
   * amount of changes we do to the global audio processor since it can
   * cause "boom" noises in the speakers.
   ****************************************************************************/


  if (g_IntelSMDGlobals.GetAudioProcessor() == -1)
  {
    initOutput = true;
  }
  else
  {

    if (g_IntelSMDGlobals.GetAudioOutputCount() == -1 ||
        output_config.ch_config != m_outputChannelConfig ||
        output_config.ch_map != m_outputChannelMap ||
        output_config.out_mode != m_outputOutMode ||
        output_config.sample_rate != m_outputSampleRate ||
        output_config.sample_size != m_ioBitsPerSample ||
        bAllOutputs != m_bAllOutputs || 
        !g_IntelSMDGlobals.GetAudioOutputAdded(audioOutputMode))
    {
      initOutput = true;
    }
  }

  m_owner = this;

  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize %d initOutput %d\n", initOutput);

  bool bAudioOnAllSpeakers(false);
  g_audioContext.SetupSpeakerConfig(iChannels, bAudioOnAllSpeakers, bIsMusic);
  g_audioContext.SetActiveDevice(CAudioContext::DIRECTSOUND_DEVICE);

  m_bIsInit = false;

  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::Initialize bTimed = %d\n", m_bTimed);
  //printf("CIntelSMDAudioRenderer::Initialize bTimed = %d\n", m_bTimed);


  audioProcessor = g_IntelSMDGlobals.GetAudioProcessor();
  audioDev = g_IntelSMDGlobals.GetAudioDevice(m_bTimed);
  audio_inputPort = g_IntelSMDGlobals.GetAudioInput(m_bTimed);

  if(audioProcessor == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize m_audioProcessor == -1");
    return false;
  }
  
  /****************************************************************************
   * Input port management
   ****************************************************************************/
  ismd_result_t result;

  if (audioDev == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize m_audioDev == -1");
    return false;
  }

  if (audio_inputPort == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize m_inputPort == -1");
    return false;
  }

  result = ISMD_ERROR_UNSPECIFIED;
  int counter = 0;

  while(counter < 5)
  {
    result = ismd_audio_input_set_data_format(audioProcessor, audioDev, ismdAudioInputFormat);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "ismd_audio_input_set_data_format failed\n");
      counter++;
      Sleep(100);
    } else
      break;
  }

  if(result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error set input data format: %d\n", result);
    return false;
  }

  if (ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_PCM)
  {
    result = ismd_audio_input_set_pcm_format(audioProcessor, audioDev, uiBitsPerSample, uiSamplesPerSec, inputChannelConfig);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_input_set_pcm_format: %d", result);
      return false;
    }
  }
  else if (!bUsePassthrough)
  {
    // Input is not PCM and we are not doing passthrough. Configure decoder...
    switch (ismdAudioInputFormat)
    {
    case ISMD_AUDIO_MEDIA_FMT_DD_PLUS:
    {
      if (g_advancedSettings.m_ddplus.rfmode)
      {
        ismd_audio_ddplus_dynamic_range_compression_mode_t val = ISMD_AUDIO_DDPLUS_RF_REMOTE_MODE;
        result = ismd_audio_input_set_decoder_param(audioProcessor, audioDev,
            ISMD_AUDIO_DDPLUS_DYNAMIC_RANGE_COMPRESSION_MODE,
            (ismd_audio_decoder_param_t*) &val);
        if (result != ISMD_SUCCESS)
        {
          CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_input_set_decoder_param rfmode: %d", result);
          return false;
        }
      }
      if (!g_advancedSettings.m_ddplus.lfemode)
      {
        ismd_audio_ddplus_lfe_channel_output_t val = ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT_NONE;
        result = ismd_audio_input_set_decoder_param(audioProcessor, audioDev,
            ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT,
            (ismd_audio_decoder_param_t*) &val);

        if (result != ISMD_SUCCESS)
        {
          CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_input_set_decoder_param lfemode: %d", result);
          return false;
        }
      }
      if (g_advancedSettings.m_ddplus.drc)
      {
        // DRC seems to be controlled by rfmode == true only ? no separate control for this ?
        CLog::Log(LOGDEBUG, "DD+ certification: not setting DRC as this option is not configurable");
      }
      if (g_advancedSettings.m_ddplus.monomode != CAdvancedSettings::__ddplus::_stereo)
      {
        ismd_audio_ddplus_dual_mono_reproduction_mode_t val;

        if (g_advancedSettings.m_ddplus.monomode == CAdvancedSettings::__ddplus::_left)
          val = ISMD_AUDIO_DDPLUS_DUAL_MONO_REPRODUCTION_MODE_LEFT_MONO;
        else if (g_advancedSettings.m_ddplus.monomode == CAdvancedSettings::__ddplus::_right)
          val = ISMD_AUDIO_DDPLUS_DUAL_MONO_REPRODUCTION_MODE_RIGHT_MONO;
        if (g_advancedSettings.m_ddplus.monomode == CAdvancedSettings::__ddplus::_mixed)
          val = ISMD_AUDIO_DDPLUS_DUAL_MONO_REPRODUCTION_MODE_MIXED_MONO;

        result = ismd_audio_input_set_decoder_param(audioProcessor, audioDev, ISMD_AUDIO_DDPLUS_DUAL_MONO_REPRODUCTION_MODE,
            (ismd_audio_decoder_param_t*) &val);

        if (result != ISMD_SUCCESS)
        {
          CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_input_set_decoder_param monomode: %d", result);
          return false;
        }
      }
      break;
    }
    default:
      break;
    };
  }

  if (passthrough_config.is_pass_through)
  {
    result = ismd_audio_input_set_as_primary(audioProcessor, audioDev, passthrough_config);
    if (result)
    {
      CLog::Log(LOGERROR, "ismd_audio_input_set_as_primary failed");
      return false;
    }
  }

  // This function will prevent bad pts packets from dropping.
  // The tradeoff is AV sync.
  int ahead = 40;
  int behind =  80;
  if(bAllowPoorAccuracy)
  {
    result = ismd_audio_input_set_timing_accuracy(audioProcessor, audioDev, ahead, behind);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDGlobals::CreateAudioDevice ismd_audio_input_set_timing_accuracy failed %d", result);
      return false;
    }
  }

  // set up decoder parameters according to vendors certifications requests
  if(ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DD)
    ConfigureDolbyModes(audioProcessor, audioDev);
  else if(ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DD_PLUS)
      ConfigureDolbyPlusModes(audioProcessor, audioDev, bAC3Encode);
  else if(ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_TRUE_HD)
      ConfigureDolbyTrueHDModes(audioProcessor, audioDev);
  else if(ismdAudioInputFormat == ISMD_AUDIO_MEDIA_FMT_DTS)
      ConfigureDTSModes(audioProcessor, audioDev);

  /****************************************************************************
   * Output port management
   ****************************************************************************/

  int audio_output_count = g_IntelSMDGlobals.GetAudioOutputCount();

  if (initOutput && audio_output_count != -1)
  {
    g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_STOP, m_bTimed);
    if(!g_IntelSMDGlobals.RemoveAudioOutput())
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error remove output");
      return false;
    }
    audio_output_count = g_IntelSMDGlobals.GetAudioOutputCount();
  }

  m_nSampleRate = output_config.sample_rate;

  if (audio_output_count == -1)
  {
    if( bAllOutputs )
    {
      int outputs[3] = {AUDIO_ANALOG, AUDIO_DIGITAL_SPDIF, AUDIO_DIGITAL_HDMI};
      bool bOne = false;
      for( int i = 0; i < 3; i++ )
      {
        if(!g_IntelSMDGlobals.AddAudioOutput(outputs[i], output_config))
        {
          CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error add output");
        }
        else
        {
          // at least one output initialized properly
          bOne = true;
        }
        
        // fail only if we couldn't initialize one output
        // this handles the case where no audio sink exists on the hdmi hookup
        if( !bOne )
        {
          CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - no audio sink exists on the hdmi hookup");
          return false;
        }
      }
    }
    else
    {
      if(!g_IntelSMDGlobals.AddAudioOutput(audioOutputMode, output_config))
      {
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error add output");
        return false;
      }
    }
  }

  // set delay on all outputs. Required by dolby cert.
  for (int i = 0; i < 3; i++)
  {
    if (g_IntelSMDGlobals.GetAudioOutputAdded(i))
    {
      ismd_audio_output_t audio_output = -1;
      audio_output = g_IntelSMDGlobals.GetAudioOutput(i);

      result = ismd_audio_output_set_delay(audioProcessor, audio_output, AUDIO_OUTPUT_DELAY);
      if (result != ISMD_SUCCESS)
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_output_set_delay %d failed %d", AUDIO_OUTPUT_DELAY, result);

      int output_delay = -1;
      ismd_audio_output_get_delay(audioProcessor, audio_output, &output_delay);
      if(output_delay != AUDIO_OUTPUT_DELAY)
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - ismd_audio_output_get_delay error. requested %d actual %d",
            AUDIO_OUTPUT_DELAY, output_delay);
    }
  }

  // Check if downmix mode changed
  ismd_audio_downmix_mode_t downmixMode = (ismd_audio_downmix_mode_t) g_guiSettings.GetInt("audiooutput.downmix");
  if(m_DownmixMode != downmixMode)
  {
    initOutput = true;
    m_DownmixMode = downmixMode;
  }

  if (initOutput)
  {
    if(!g_IntelSMDGlobals.DisableAudioOutput())
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error disable output");
      return false;
    }

    ismd_audio_clk_src_t clockSource;
    soc_user_info_t info;
    int intResult;
    if ((intResult = system_utils_get_soc_info(&info)) == 0)
    {
      if (info.name_enum == SOC_NAME_CE3100)
      {
        clockSource = ISMD_AUDIO_CLK_SRC_INTERNAL;
      }
      else if (info.name_enum == SOC_NAME_CE4100)
      {
        clockSource = ISMD_AUDIO_CLK_SRC_EXTERNAL;
      }
      else
      {
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - unknown SOC");
        return false;
      }
    }
    else
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error get soc info: %d", result);
      return false;
    }

    unsigned int clockrate = 0;
    switch (output_config.sample_rate)
    {
    case 12000:
    case 32000:
    case 48000:
    case 96000:
    case 192000:
      clockrate = 36864000;
      break;
    case 44100:
    case 88200:
    case 176400:
      clockrate = 33868800;
      break;
    default:
      break;
    }

    if( clockrate && (result = ismd_audio_configure_master_clock(audioProcessor, clockrate, clockSource)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - configure_audio_master_clock %d failed (%d)!", clockrate, result);
      return false;
    }

    CLog::Log(LOGNONE, "Setting downmix mode to %d", m_DownmixMode);

    if (g_IntelSMDGlobals.GetAudioOutputAdded(AUDIO_ANALOG))
    {
      ismd_audio_output_t outp = g_IntelSMDGlobals.GetAudioOutput(AUDIO_ANALOG);
      result = ismd_audio_output_set_downmix_mode(audioProcessor, outp, m_DownmixMode);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error setting LTRT downmix %d", result);
      }
    }
    if (g_IntelSMDGlobals.GetAudioOutputAdded(AUDIO_DIGITAL_SPDIF))
    {
      ismd_audio_output_t outp = g_IntelSMDGlobals.GetAudioOutput(AUDIO_DIGITAL_SPDIF);
      result = ismd_audio_output_set_downmix_mode(audioProcessor, outp, m_DownmixMode);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error setting LTRT downmix %d", result);
      }
    }
    if (g_IntelSMDGlobals.GetAudioOutputAdded(AUDIO_DIGITAL_HDMI)
        && output_config.ch_config == ISMD_AUDIO_STEREO)
    {
      ismd_audio_output_t outp = g_IntelSMDGlobals.GetAudioOutput(AUDIO_DIGITAL_HDMI);
      result = ismd_audio_output_set_downmix_mode(audioProcessor, outp, m_DownmixMode);
      if (result != ISMD_SUCCESS)
      {
        CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - error setting LTRT downmix %d", result);
      }
    }

    if(!g_IntelSMDGlobals.EnableAudioOutput())
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - enable output error");
      return false;
    }
  }

  //
  // Store our state
  //

  m_bPause = false;
  m_uiChannels = iChannels;
  m_channelMap = channelMap;
  m_uiSamplesPerSec = uiSamplesPerSec;
  m_uiBitsPerSample = uiBitsPerSample;
  m_bPassthrough = bUsePassthrough;
  m_bAllOutputs = bAllOutputs;
  m_audioMediaFormat = audioMediaFormat;
  m_inputChannelConfig = inputChannelConfig;
  m_outputChannelConfig = output_config.ch_config;
  m_outputChannelMap = output_config.ch_map;
  m_outputOutMode = output_config.out_mode;
  m_outputSampleRate = output_config.sample_rate;
  m_uiBytesPerSecond = uiSamplesPerSec * (uiBitsPerSample / 8) * iChannels;
  m_nCurrentVolume = g_stSettings.m_nVolumeLevel;
  m_inputAudioFormat = ismdAudioInputFormat;
  m_ioBitsPerSample = m_uiBitsPerSample;
  m_inputSamplesPerSec = m_uiSamplesPerSec;

  m_lastPts = ISMD_NO_PTS;

  g_IntelSMDGlobals.EnableAudioInput(m_bTimed);

  if(!m_bTimed)
  {
    g_IntelSMDGlobals.SetBaseTime(0);
    g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_PLAY, false);
    m_bFlushFlag = false;
  }
  else
  {
    g_IntelSMDGlobals.SetAudioDeviceBaseTime(g_IntelSMDGlobals.GetBaseTime());
    g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_PAUSE, true);
    m_bFlushFlag = true;
  }

  // If we aren't doing pcm, set the volume to max (disable the mixer)
  // If we are doing PCM, restore to the last volume setting
  if( m_bPassthrough )
    g_IntelSMDGlobals.SetMasterVolume(VOLUME_MAXIMUM);
  else
    g_IntelSMDGlobals.SetMasterVolume(m_nCurrentVolume);

  if(m_pChunksCollected)
    delete m_pChunksCollected;

  m_pChunksCollected = new unsigned char[m_dwBufferLen];
  m_first_pts = ISMD_NO_PTS;
  m_lastPts = ISMD_NO_PTS;
  m_ChunksCollectedSize = 0;

  m_bIsAllocated = true;
  m_bIsInit = true;

  CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Initialize done");

  return true;
}

CIntelSMDAudioRenderer::~CIntelSMDAudioRenderer()
{
  Deinitialize();
}

static const int _default_channel_layouts[] =
  { AUDIO_CHAN_CONFIG_2_CH,     // if channel count is 0, assume stereo
    AUDIO_CHAN_CONFIG_2_CH,     // for PCM 1 channel we map to 2 channels
    AUDIO_CHAN_CONFIG_2_CH,
    0xFFFFF210,                 // 3CH: Left, Center, Right
    0xFFFF4320,                 // 4CH: Left, Right, Left Sur, Right Sur
    0xFFF43210,                 // 5CH: Left, Center, Right, Left Sur, Right Sur
    AUDIO_CHAN_CONFIG_6_CH,
    0xF6543210,                 // 7CH: Left, Center, Right, Left Sur, Right Sur, Left Rear Sur, Right Rear Sur
    AUDIO_CHAN_CONFIG_8_CH
  };
//static 
int CIntelSMDAudioRenderer::BuildChannelConfig( enum PCMChannels* channelMap, int iChannels )
{
  int res = 0;

  // SMD will have major issues should this ever occur.
  if( iChannels > 8 )
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::BuildChannelConfig received more than 8 channels? (%d)", iChannels);
    return 0;
  }

  if( !channelMap )
  {
    // if we don't have a mapping, it's guesswork. how do 4ch or 5ch streams work? who knows.
    // use some defaults from intel and try some other things here
    // mono and stereo are probably always right, though AUDIO_CHAN_CONFIG_1_CH is configured as center-only

    res = _default_channel_layouts[iChannels];

    if( iChannels > 2 )
      CLog::Log(LOGWARNING, "CIntelSMDAudioRenderer::BuildChannelConfig - using a default configuration for %d channels; your channel mapping may be wrong\n", iChannels);
  }
  else
  {
    int i;
    bool bBackToSur = false;     // if we are outputing 5.1 we may have to remap the back channels to surround
    bool bUsingSides = false;    // if we use the side channels this is true
    bool bUsingOffC = false;     // if we use left/right of center channels this is true
    for( i = 0; i < iChannels; i++ )
    {
      int val = -1;

      // PCM remap - we assume side channels are surrounds, but also support left/right of center being surrounds
      //  we can't, however, handle both channel sets being present in a stream.
      switch( channelMap[i] )
      {
        case PCM_FRONT_LEFT:    val = ISMD_AUDIO_CHANNEL_LEFT; break;
        case PCM_FRONT_RIGHT:   val = ISMD_AUDIO_CHANNEL_RIGHT; break;
        case PCM_FRONT_CENTER:  val = (iChannels > 1) ? ISMD_AUDIO_CHANNEL_CENTER : ISMD_AUDIO_CHANNEL_LEFT; break;
        case PCM_LOW_FREQUENCY: val = ISMD_AUDIO_CHANNEL_LFE; break;
        case PCM_BACK_LEFT:
        {
          if( iChannels == 8 )
            val = ISMD_AUDIO_CHANNEL_LEFT_REAR_SUR;
          else
          {
            val = ISMD_AUDIO_CHANNEL_LEFT_SUR;
            bBackToSur = true;
          }
          break;
        }
        case PCM_BACK_RIGHT:
        {
          if( iChannels == 8 )
            val = ISMD_AUDIO_CHANNEL_RIGHT_REAR_SUR;
          else
          {
            val = ISMD_AUDIO_CHANNEL_RIGHT_SUR;
            bBackToSur = true;
          }
          break;
        }
        case PCM_FRONT_LEFT_OF_CENTER:  val = ISMD_AUDIO_CHANNEL_LEFT_SUR; bUsingOffC = true; break;
        case PCM_FRONT_RIGHT_OF_CENTER: val = ISMD_AUDIO_CHANNEL_RIGHT_SUR; bUsingOffC = true; break;
        case PCM_BACK_CENTER:           break;   // not supported
        case PCM_SIDE_LEFT:     val = ISMD_AUDIO_CHANNEL_LEFT_SUR; bUsingSides = true; break;
        case PCM_SIDE_RIGHT:    val = ISMD_AUDIO_CHANNEL_RIGHT_SUR; bUsingSides = true; break;
        case PCM_TOP_FRONT_LEFT:        break;   // not supported
        case PCM_TOP_FRONT_RIGHT:       break;   // not supported
        case PCM_TOP_FRONT_CENTER:      break;   // not supported
        case PCM_TOP_CENTER:            break;   // not supported
        case PCM_TOP_BACK_LEFT:         break;   // not supported
        case PCM_TOP_BACK_RIGHT:        break;   // not supported
        case PCM_TOP_BACK_CENTER:       break;   // not supported
        default: break;
      };

      if( val == -1 )
      {
        CLog::Log(LOGWARNING, "CIntelSMDAudioRenderer::BuildChannelConfig - invalid channel %d\n", channelMap[i] );
        break;
      }

      if( bUsingSides && bUsingOffC )
      {
        CLog::Log(LOGWARNING, "CIntelSMDAudioRenderer::BuildChannelConfig - given both side and off center channels - aborting");
        break;
      }

      res |= (val << (4*i));
    }

    if( i < iChannels )
    {
      // Here we have a problem. The source content has a channel layout that we couldn't convert, so at this point we are better off using a
      // default channel layout. We'll never get the layout to be accurate anyways, and at least this way SMD is seeing the right amount of PCM...
      CLog::Log(LOGWARNING, "CIntelSMDAudioRenderer::BuildChannelConfig - switching back to default channel layout since not all channels were found");
      res = _default_channel_layouts[iChannels];
    }
    else
    {
      if( bBackToSur )
      {
        CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::BuildChannelConfig - remapping back channels to surround speakers for 5.1 audio out");
      }
      // mark the remaining slots invalid
      for( int i = iChannels; i < 8; i++ )
      {
        res |= (ISMD_AUDIO_CHANNEL_INVALID << (i*4));
      }
    }
  }

  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer::BuildChannelConfig - %d channels layed out as 0x%08x\n", iChannels, res);
  return res;
}

void CIntelSMDAudioRenderer::InitializeIfNeeded(bool force)
{
  if (m_owner == this && !force)
  {
    return;
  }

  CLog::Log(LOGDEBUG, "IntelSMDAudioRenderer::InitializeIfNeeded is required\n");

  Initialize(m_pCallback, m_uiChannels, m_channelMap, m_uiSamplesPerSec, m_uiBitsPerSample, false, "reinit", false, m_bPassthrough, m_bTimed, m_audioMediaFormat);
}

bool CIntelSMDAudioRenderer::Deinitialize()
{
  ismd_result_t result;

  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer %s\n", __FUNCTION__);

  // If there is no gui sound, our audio device might get stuck in passthrough mode.
  // External apps (youtube, netflix) don't work in passthrough mode, so we need to revert.
  // If gui is enables, we will revert anyway
  if (g_guiSettings.GetString("lookandfeel.soundskin")=="OFF")
  {
    CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Deinitialize gui sound is off. Reverting device to 48k PCM", __FUNCTION__);
    Initialize(NULL, 2, NULL, 48000, 16, false, "gui", true, false);
  }

  g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_STOP, m_bTimed);
  g_IntelSMDGlobals.FlushAudioDevice(m_bTimed);

  m_bIsInit = false;

  return true;
}

void CIntelSMDAudioRenderer::Flush() 
{
  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer %s", __FUNCTION__);

  InitializeIfNeeded();

  ismd_result_t result;

  g_IntelSMDGlobals.FlushAudioDevice(m_bTimed);

  if(m_bTimed)
    m_bFlushFlag = true;

  m_lastPts = ISMD_NO_PTS;
  m_lastSync = ISMD_NO_PTS;
}

void CIntelSMDAudioRenderer::Resync(double pts)
{

}

bool CIntelSMDAudioRenderer::Pause()
{
  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer %s\n", __FUNCTION__);

  InitializeIfNeeded();

  // We need to wait until we're done with our packets
  CSingleLock lock(m_SMDAudioLock);

  if (!m_bIsAllocated)
    return false;

  if (m_bPause)
    return true;

  g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_PAUSE, m_bTimed);

  m_bPause = true;

  return true;
}

//***********************************************************************************************
bool CIntelSMDAudioRenderer::Resume()
{
  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer %s\n", __FUNCTION__);

  InitializeIfNeeded();

  if (!m_bIsAllocated)
    return false;

  if(!m_bFlushFlag)
  {
    g_IntelSMDGlobals.SetAudioDeviceBaseTime(g_IntelSMDGlobals.GetBaseTime());
    g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_PLAY, m_bTimed);
  }
  else
    CLog::Log(LOGINFO, "CIntelSMDAudioRenderer::Resume flush flag is set, ignoring request\n");

  m_bPause = false;

  return true;
}

//***********************************************************************************************
bool CIntelSMDAudioRenderer::Stop()
{
  CLog::Log(LOGDEBUG, "CIntelSMDAudioRenderer %s\n", __FUNCTION__);

  g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_STOP, m_bTimed);
  g_IntelSMDGlobals.FlushAudioDevice(m_bTimed);

  return true;
}

//***********************************************************************************************
long CIntelSMDAudioRenderer::GetCurrentVolume() const
{
  return m_nCurrentVolume;
}

//***********************************************************************************************
void CIntelSMDAudioRenderer::Mute(bool bMute)
{
  g_IntelSMDGlobals.Mute(bMute);
}

//***********************************************************************************************
bool CIntelSMDAudioRenderer::SetCurrentVolume(long nVolume)
{
  InitializeIfNeeded();

  // If we fail or if we are doing passthrough, don't set
  if (!m_bIsAllocated || m_bPassthrough )
    return false;

  m_nCurrentVolume = nVolume;
  return g_IntelSMDGlobals.SetMasterVolume(nVolume);
}


//***********************************************************************************************
unsigned int CIntelSMDAudioRenderer::GetSpace()
{
  InitializeIfNeeded();
  unsigned int curDepth = 0;
  unsigned int maxDepth = 0;

  if (!m_bIsAllocated)
  {
    return 0;
  }

  CSingleLock lock(m_SMDAudioLock);
  ismd_port_status_t portStatus;

  ismd_port_handle_t inputPort = g_IntelSMDGlobals.GetAudioInput(m_bTimed);
  if(inputPort == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::GetSpace - inputPort == -1");
    return 0;
  }

  ismd_result_t smd_ret = g_IntelSMDGlobals.GetPortStatus(inputPort, curDepth, maxDepth);

  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::GetSpace - error getting port status: %d", smd_ret);
    return 0;
  }

  //printf("max depth = %d cur depth = %d\n", portStatus.max_depth,  portStatus.cur_depth);
  unsigned int result = (maxDepth - curDepth) * m_dwBufferLen;
  return result;
}

unsigned int CIntelSMDAudioRenderer::SendDataToInput(unsigned char* buffer_data, unsigned int buffer_size, ismd_pts_t local_pts)
{
  ismd_result_t smd_ret;
  ismd_buffer_handle_t ismdBuffer;
  ismd_es_buf_attr_t *buf_attrs;
  ismd_buffer_descriptor_t ismdBufferDesc;
  ismd_port_handle_t inputPort;


  if(m_dwBufferLen < buffer_size)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput data size %d is bigger that smd buffer size %d\n",
        buffer_size, m_dwBufferLen);
    return 0;
  }

  inputPort = g_IntelSMDGlobals.GetAudioInput(m_bTimed);
  if(inputPort == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput - inputPort == -1");
    return 0;
  }

  smd_ret = ismd_buffer_alloc(m_dwBufferLen, &ismdBuffer);
  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput - error allocating buffer: %d", smd_ret);
    return 0;
  }

  smd_ret = ismd_buffer_read_desc(ismdBuffer, &ismdBufferDesc);
  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput - error reading descriptor: %d", smd_ret);
    ismd_buffer_dereference(ismdBuffer);
    return 0;
  }

  unsigned char* buf_ptr = (uint8_t *) OS_MAP_IO_TO_MEM_NOCACHE(ismdBufferDesc.phys.base, buffer_size);
  if(buf_ptr == NULL)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput - unable to mmap buffer %p", ismdBufferDesc.phys.base);
    ismd_buffer_dereference(ismdBuffer);
    return 0;    
  }

  memcpy(buf_ptr, buffer_data, buffer_size);
  OS_UNMAP_IO_FROM_MEM(buf_ptr, buffer_size);

  ismdBufferDesc.phys.level = buffer_size;

  buf_attrs = (ismd_es_buf_attr_t *) ismdBufferDesc.attributes;
  buf_attrs->original_pts = local_pts;
  buf_attrs->local_pts = local_pts;
  buf_attrs->discontinuity = false;

  smd_ret = ismd_buffer_update_desc(ismdBuffer, &ismdBufferDesc);
  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput - error updating descriptor: %d", smd_ret);
    ismd_buffer_dereference(ismdBuffer);
    return 0;
  }

  int counter = 0;
  while (counter < 20)
  {
    smd_ret = ismd_port_write(inputPort, ismdBuffer);
    if (smd_ret != ISMD_SUCCESS)
    {
      if(g_IntelSMDGlobals.GetAudioDeviceState(m_bTimed) != ISMD_DEV_STATE_STOP)
      {
        counter++;
        Sleep(100);
      }
      else
      {
        break;
      }
    }
    else
      break;
  }

  if(smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::SendDataToInput failed to write buffer %d\n", smd_ret);
    ismd_buffer_dereference(ismdBuffer);
    buffer_size = 0;
  }

  return buffer_size;
}

//***********************************************************************************************
unsigned int CIntelSMDAudioRenderer::AddPackets(const void* data, unsigned int len, double pts, double duration)
{
  InitializeIfNeeded();

  CSingleLock lock(m_SMDAudioLock);

  ismd_pts_t local_pts = ISMD_NO_PTS;
  unsigned char* pBuffer = NULL;
  unsigned int total = 0;
  unsigned int dataSent = 0;
  if (!m_bIsAllocated)
  {
    CLog::Log(LOGERROR,"CIntelSMDAudioRenderer::AddPackets - sanity failed. no valid play handle!");
    return len;
  }

  // don't do anything if demuxer is connected directly to decoder
  if(g_IntelSMDGlobals.IsDemuxToAudio())
    return len;

  if (m_bTimed)
  {
    local_pts = g_IntelSMDGlobals.DvdToIsmdPts(pts);
  }

  if(m_bFlushFlag && m_bTimed)
  {
    ismd_pts_t startPTS;

    if(g_IntelSMDGlobals.GetAudioStartPts() != ISMD_NO_PTS)
      startPTS = g_IntelSMDGlobals.GetAudioStartPts();
    else
      startPTS = local_pts;

    if(g_IntelSMDGlobals.GetFlushFlag())
    {
      g_IntelSMDGlobals.SetBaseTime(g_IntelSMDGlobals.GetCurrentTime());
      g_IntelSMDGlobals.SetFlushFlag(false);
    }

    g_IntelSMDGlobals.SetAudioStartPts(startPTS);
    g_IntelSMDGlobals.SetAudioDeviceBaseTime(g_IntelSMDGlobals.GetBaseTime());
    g_IntelSMDGlobals.SendStartPacket(startPTS, g_IntelSMDGlobals.GetAudioInput(m_bTimed));
    g_IntelSMDGlobals.SetAudioDeviceState(ISMD_DEV_STATE_PLAY, m_bTimed);

    // data for small chunks collection
    m_ChunksCollectedSize = 0;
    m_bFlushFlag = false;
  }

  pBuffer = (unsigned char*)data;
  total = len;

  // What's the maximal write size - either a chunk if provided, or the full buffer
  unsigned int block_size = (m_dwChunkSize ? m_dwChunkSize : m_dwBufferLen);

  while (len && len >= m_dwChunkSize) // We want to write at least one chunk at a time
  {
    unsigned int bytes_to_copy = len > block_size ? block_size : len;

    // If we've written a frame with this pts, don't write another
    if(local_pts == m_lastPts)
    {
      local_pts = ISMD_NO_PTS;
    }

    if(m_ChunksCollectedSize == 0)
      m_first_pts = local_pts;

    // collect data to avoid small chunks
    if (m_bTimed)
    {
      if (m_ChunksCollectedSize + bytes_to_copy <= m_dwBufferLen)
      {
        memcpy(m_pChunksCollected + m_ChunksCollectedSize, pBuffer,
            bytes_to_copy);
      }
      else
      {
        //printf("Audio Packet: PTS %lld (0x%09lx)\n", m_first_pts, m_first_pts);
        SendDataToInput(m_pChunksCollected, m_ChunksCollectedSize, m_first_pts);
        memcpy(m_pChunksCollected, pBuffer, bytes_to_copy);
        m_ChunksCollectedSize = 0;
        m_first_pts = local_pts;
      }
      m_ChunksCollectedSize += bytes_to_copy;
      dataSent = bytes_to_copy;
    }
    else
    {
      dataSent = SendDataToInput(pBuffer, bytes_to_copy, local_pts);
    }

    if(dataSent != bytes_to_copy)
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::AddPackets SendDataToInput failed. req %d actual %d", len, dataSent);
      return 0;
    }

    pBuffer += dataSent; // Update buffer pointer
    len -= dataSent; // Update remaining data len

    if( local_pts != ISMD_NO_PTS )
      m_lastPts = local_pts;
  }

  return total - len; // Bytes used
}

//***********************************************************************************************
float CIntelSMDAudioRenderer::GetDelay()
{
  return 0.0f;
}

float CIntelSMDAudioRenderer::GetCacheTime()
{
  unsigned int curDepth = 0;
  unsigned int maxDepth = 0;

  InitializeIfNeeded();

  if (!m_bIsAllocated)
  {
    return 0.0f;
  }

  ismd_port_handle_t inputPort = g_IntelSMDGlobals.GetAudioInput(m_bTimed);
  if(inputPort == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::GetCacheTime - inputPort == -1");
    return 0;
  }

  CSingleLock lock(m_SMDAudioLock);

  ismd_result_t smd_ret = g_IntelSMDGlobals.GetPortStatus(inputPort, curDepth, maxDepth);

  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::GetCacheTime - error getting port status: %d", smd_ret);
    return 0;
  }

  float result = ((float) (curDepth * m_dwBufferLen)) / (float) m_uiBytesPerSecond;

  return result;
}

unsigned int CIntelSMDAudioRenderer::GetChunkLen()
{
  return m_dwChunkSize;
}

int CIntelSMDAudioRenderer::SetPlaySpeed(int iSpeed)
{
  return 0;
}

void CIntelSMDAudioRenderer::RegisterAudioCallback(IAudioCallback *pCallback)
{
  m_pCallback = pCallback;
}

void CIntelSMDAudioRenderer::UnRegisterAudioCallback()
{
  m_pCallback = NULL;
}

void CIntelSMDAudioRenderer::WaitCompletion()
{
  unsigned int curDepth = 0;
  unsigned int maxDepth = 0;

  InitializeIfNeeded();

  if (!m_bIsAllocated || m_bPause)
    return;

  ismd_port_handle_t inputPort = g_IntelSMDGlobals.GetAudioInput(m_bTimed);
  if(inputPort == -1)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::GetSpace - inputPort == -1");
    return;
  }

  CSingleLock lock(m_SMDAudioLock);

  ismd_result_t smd_ret = g_IntelSMDGlobals.GetPortStatus(inputPort, curDepth, maxDepth);
  if (smd_ret != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::WaitCompletion - error getting port status: %d", smd_ret);
    return;
  }

  while (curDepth != 0)
  {
    usleep(5000);

    ismd_result_t smd_ret = g_IntelSMDGlobals.GetPortStatus(inputPort, curDepth, maxDepth);
    if (smd_ret != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::WaitCompletion - error getting port status: %d", smd_ret);
      return;
    }
  } 
}

void CIntelSMDAudioRenderer::SwitchChannels(int iAudioStream, bool bAudioOnAllSpeakers)
{
  return;
}

// Based on ismd examples
bool CIntelSMDAudioRenderer::LoadEDID()
{
  bool ret = false;
  gdl_hdmi_audio_ctrl_t ctrl;
  edidHint** cur = &m_edidTable;

  // Set up our control
  ctrl.cmd_id = GDL_HDMI_AUDIO_GET_CAPS;
  ctrl.data._get_caps.index = 0;

  // Clear our bitstream support table
  g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_AC3,    false );
  g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_EAC3,   false );
  g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_TRUEHD, false );
  g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_DTS,    false );
  g_IntelSMDGlobals.SetCodecHDMIBitstream( SMD_CODEC_DTSHD, false );

  while( gdl_port_recv(GDL_PD_ID_HDMI, GDL_PD_RECV_HDMI_AUDIO_CTRL, &ctrl, sizeof(ctrl)) == GDL_SUCCESS )
  {
    edidHint* hint = new edidHint;
    if( !hint ) return false;

    ret = true;

    hint->format = MapGDLAudioFormat( (gdl_hdmi_audio_fmt_t)ctrl.data._get_caps.cap.format );
    if( ISMD_AUDIO_MEDIA_FMT_INVALID == hint->format )
    {
      delete hint;
      ctrl.data._get_caps.index++;
      continue;
    }
    else
    {
      // For any formats we support bitstreaming on, flag them in the smdglobals here
      switch( hint->format )
      {
        case ISMD_AUDIO_MEDIA_FMT_DD:
          g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_AC3, true);
          break;
        case ISMD_AUDIO_MEDIA_FMT_DD_PLUS:
          g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_EAC3, true );
          break;
        case ISMD_AUDIO_MEDIA_FMT_TRUE_HD:
          g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_TRUEHD, true );
          break;
        case ISMD_AUDIO_MEDIA_FMT_DTS:
          g_IntelSMDGlobals.SetCodecHDMIBitstream( CODEC_ID_DTS, true );
          break;
        case ISMD_AUDIO_MEDIA_FMT_DTS_HD_MA:
          g_IntelSMDGlobals.SetCodecHDMIBitstream( SMD_CODEC_DTSHD, true );
          break;
        default:
          break;
      };
    }
    
    hint->channels = (int)ctrl.data._get_caps.cap.max_channels;
    hint->sample_rates = (unsigned char) (ctrl.data._get_caps.cap.fs & 0x7f);
    if( ISMD_AUDIO_MEDIA_FMT_PCM == hint->format )
    {
      hint->sample_sizes = (ctrl.data._get_caps.cap.ss_bitrate & 0x07);
      if( hint->sample_sizes & 0x04 )
        hint->sample_sizes |= 0x08;  // if we support 24, we support 32.
    }
    else
      hint->sample_sizes = 0;

    *cur = hint;
    cur = &( hint->next );

    // get the next block
    ctrl.data._get_caps.index++;
  }

  *cur = NULL;

  DumpEDID();

  return ret;
}
void CIntelSMDAudioRenderer::UnloadEDID()
{
  while( m_edidTable )
  {
    edidHint* nxt = m_edidTable->next;
    delete[] m_edidTable;
    m_edidTable = nxt;
  }
}
void CIntelSMDAudioRenderer::DumpEDID()
{
  char* formats[] = {"invalid","LPCM","DVDPCM","BRPCM","MPEG","AAC","AACLOAS","DD","DD+","TrueHD","DTS-HD","DTS-HD-HRA","DTS-HD-MA","DTS","DTS-LBR","WM9"};
  CLog::Log(LOGINFO,"HDMI Audio sink supports the following formats:\n");
  for( edidHint* cur = m_edidTable; cur; cur = cur->next )
  {
    CStdString line;
    line.Format("* Format: %s Max channels: %d Samplerates: ", formats[cur->format], cur->channels);

    for( int z = 0; z < sizeof(s_edidRates); z++ )
    {
      if( cur->sample_rates & (1<<z) )
      {
        line.AppendFormat("%d ",s_edidRates[z]);
      }         
    }
    if( ISMD_AUDIO_MEDIA_FMT_PCM == cur->format )
    {
      line.AppendFormat("Sample Sizes: ");
      for( int z = 0; z< sizeof(s_edidSampleSizes); z++ )
      {
        if( cur->sample_sizes & (1<<z) )
        {
          line.AppendFormat("%d ",s_edidSampleSizes[z]);
        }
      }
    }
    CLog::Log(LOGINFO, "%s", line.c_str());
  }
}

bool CIntelSMDAudioRenderer::CheckEDIDSupport( ismd_audio_format_t format, int& iChannels, unsigned int& uiSampleRate, unsigned int& uiSampleSize )
{
  //
  // If we have EDID data then see if our settings are viable
  // We have to match the format. After that, we prefer to match channels, then sample rate, then sample size
  // The result is if we have a 7.1 8 channel 192khz stream and have to pick between 2 ch at 192khz or 8 ch at
  // 48khz, we'll do the latter, and keep the surround experience for the user.
  //

  edidHint* cur = NULL;
  unsigned int suggSampleRate = uiSampleRate;
  unsigned int suggSampleSize = uiSampleSize;
  int suggChannels = iChannels;
  bool bFormatSupported = false;
  bool bFullMatch = false;

  if( !m_edidTable )
    return false;

  for( cur = m_edidTable; cur; cur = cur->next )
  {
    bool bSuggested = false;

    // Check by format and channels
    if( cur->format != format )
      continue;

    bFormatSupported = true;

    // Right format. See if the proper channel count is reflected
    // if not, we'll suggest a resample
    // if so, we update our suggestions to reflect this format entry
    if( cur->channels < iChannels )
    {
      suggChannels = cur->channels;
      bSuggested = true;
    }
    else
    {
      suggChannels = iChannels;
      suggSampleRate = uiSampleRate;
      suggSampleSize = uiSampleSize;
    }

    // Next, see if the sample rate is supported
    for( int z = 0; z < sizeof(s_edidRates); z++ )
    {
      if( s_edidRates[z] == uiSampleRate )
      {
        if( !(cur->sample_rates & (1<<z)) )
        {
          // For now, try force resample to 48khz and worry if that isn't supported
          if( !(cur->sample_rates & 4) )
            CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - your audio sink indicates it doesn't support 48khz"); 
          suggSampleRate = 48000;
          bSuggested = true;
        }
        break;
      }
    }

    // Last, see if the sample size is supported for PCM
    if( ISMD_AUDIO_MEDIA_FMT_PCM == format )
    {
      for( int y = 0; y < sizeof(s_edidSampleSizes); y++ )
      {
        if( s_edidSampleSizes[y] == uiSampleSize )
        {
          if( !(cur->sample_sizes & (1<<y)) )
          {
            // For now, try force resample to 16khz and worry if that isn't supported
            if( !(cur->sample_rates & 1) )
              CLog::Log(LOGERROR, "CIntelSMDAudioRenderer::Initialize - your audio sink indicates it doesn't support 16bit/sample");
            suggSampleSize = 16;
            bSuggested = true;
          }
          break;
        }
      }
    }

    // If we didn't make any suggestions, then this was a match; exit out
    if( !bSuggested )
    {
      bFullMatch = true;
      break;
    }
  }

  if( bFormatSupported )
  {
    if( bFullMatch )
    {
      return true;
    }
    else
    {
      uiSampleRate = suggSampleRate;
      uiSampleSize = suggSampleSize;
      iChannels = suggChannels;
    }
  }
  return false;
}

//static
ismd_audio_format_t CIntelSMDAudioRenderer::MapGDLAudioFormat( gdl_hdmi_audio_fmt_t f )
{
  ismd_audio_format_t result = ISMD_AUDIO_MEDIA_FMT_INVALID;
  switch(f)
  {
    case GDL_HDMI_AUDIO_FORMAT_PCM:
      result = ISMD_AUDIO_MEDIA_FMT_PCM;
      break;
    case GDL_HDMI_AUDIO_FORMAT_MPEG1:
    case GDL_HDMI_AUDIO_FORMAT_MPEG2:
    case GDL_HDMI_AUDIO_FORMAT_MP3:
      result = ISMD_AUDIO_MEDIA_FMT_MPEG;
      break;
    case GDL_HDMI_AUDIO_FORMAT_AAC:
      result = ISMD_AUDIO_MEDIA_FMT_AAC;
      break;
    case GDL_HDMI_AUDIO_FORMAT_DTS:
      result = ISMD_AUDIO_MEDIA_FMT_DTS;
      break;
    case GDL_HDMI_AUDIO_FORMAT_AC3:
      result = ISMD_AUDIO_MEDIA_FMT_DD;
      break;
    case GDL_HDMI_AUDIO_FORMAT_DDP:
      result = ISMD_AUDIO_MEDIA_FMT_DD_PLUS;
      break;
    case GDL_HDMI_AUDIO_FORMAT_DTSHD:
      result = ISMD_AUDIO_MEDIA_FMT_DTS_HD_MA;
      break;
    case GDL_HDMI_AUDIO_FORMAT_MLP:
      result = ISMD_AUDIO_MEDIA_FMT_TRUE_HD;
      break;
    case GDL_HDMI_AUDIO_FORMAT_WMA_PRO:
      result = ISMD_AUDIO_MEDIA_FMT_WM9;
      break;

  };
  return result;
}

void CIntelSMDAudioRenderer::ConfigureDolbyModes(ismd_audio_processor_t proc_handle, ismd_dev_t input_handle)
{
  ismd_result_t result;
  ismd_audio_decoder_param_t param;

  // Dolby DD is downmix to Lt/Rt and not Lo/Ro
  ismd_audio_ac3_stereo_output_mode_t *stereo_mode_dd = (ismd_audio_ac3_stereo_output_mode_t *)&param;
  *stereo_mode_dd = ISMD_AUDIO_AC3_STEREO_OUTPUT_MODE_SURROUND_COMPATIBLE;
  CLog::Log(LOGNONE, "ConfigureDolbyModes ISMD_AUDIO_AC3_STEREO_OUTPUT_MODE %d", *stereo_mode_dd);
  result = ismd_audio_input_set_decoder_param( proc_handle, input_handle,
      ISMD_AUDIO_AC3_STEREO_OUTPUT_MODE, &param );

  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyModes ISMD_AUDIO_AC3_STEREO_OUTPUT_MODE_SURROUND_COMPATIBLE failed %d",
        result);
  }

  // DD works in 2 channels output mode
  ismd_audio_ac3_num_output_channels_t *num_of_ch = (ismd_audio_ac3_num_output_channels_t *) &param;
  *num_of_ch = 2;
  CLog::Log(LOGNONE, "ConfigureDolbyModes ISMD_AUDIO_AC3_NUM_OUTPUT_CHANNELS %d", *num_of_ch);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_NUM_OUTPUT_CHANNELS, &param);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyModes ISMD_AUDIO_AC3_NUM_OUTPUT_CHANNELS failed %d", result);
  }

  // Output configuration for DD
  ismd_audio_ac3_output_configuration_t *output_config = (ismd_audio_ac3_output_configuration_t *) &param;
  *output_config = ISMD_AUDIO_AC3_OUTPUT_CONFIGURATION_2_0_LR;
  CLog::Log(LOGNONE, "ConfigureDolbyModes ISMD_AUDIO_AC3_OUTPUT_CONFIGURATION_2_0_LR %d", *output_config);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_OUTPUT_CONFIGURATION, &param);
  if (result != ISMD_SUCCESS)
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyModes ISMD_AUDIO_AC3_NUM_OUTPUT_CHANNELS failed %d", result);

  // Configure DD channels layout
  ismd_audio_ac3_channel_route_t *channel_route = (ismd_audio_ac3_channel_route_t *) &param;
  result = ISMD_SUCCESS;

  *channel_route = 0;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_CHANNEL_ROUTE_L, &param);
  *channel_route = 1;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_CHANNEL_ROUTE_R, &param);
  *channel_route = -1;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,   ISMD_AUDIO_AC3_CHANNEL_ROUTE_Ls, &param);
  *channel_route = -1;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,   ISMD_AUDIO_AC3_CHANNEL_ROUTE_Rs, &param);
  *channel_route = -1;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_CHANNEL_ROUTE_C, &param);
  *channel_route = -1;
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_AC3_CHANNEL_ROUTE_LFE, &param);

  if(result != ISMD_SUCCESS)
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyModes ISMD_AUDIO_AC3_CHANNEL_ROUTE_* failed %d", result);
}

void CIntelSMDAudioRenderer::ConfigureDolbyPlusModes(ismd_audio_processor_t proc_handle, ismd_dev_t input_handle, bool bAC3Encode)
{
  ismd_result_t result;
  ismd_audio_decoder_param_t param;

  //DD+ we need to operate in 5.1 decoding mode and avoid decoding extension channels.
  ismd_audio_ddplus_num_output_channels_t *num_of_ch = (ismd_audio_ddplus_num_output_channels_t *) &param;
  if(bAC3Encode)
    *num_of_ch = ISMD_AUDIO_DDPLUS_MAX_NUM_OUTPUT_CHANNELS;
  else
    *num_of_ch = 6;
  CLog::Log(LOGNONE, "ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_NUM_OUTPUT_CHANNELS %d", *num_of_ch);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_DDPLUS_NUM_OUTPUT_CHANNELS, &param);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_NUM_OUTPUT_CHANNELS failed %d", result);
  }

  ismd_audio_ddplus_lfe_channel_output_t *lfe_control = (ismd_audio_ddplus_lfe_channel_output_t *) &param;
  if(bAC3Encode)
    *lfe_control = ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT_ENABLED;
  else
    *lfe_control = (ismd_audio_ddplus_lfe_channel_output_t)g_guiSettings.GetInt("audiooutput.ddplus_lfe_channel_config");
  CLog::Log(LOGNONE, "ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT %d", *lfe_control);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT, &param);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_LFE_CHANNEL_OUTPUT failed %d", result);
  }

  // Dolby DD+ is downmix to Lt/Rt and not Lo/Ro
  ismd_audio_ddplus_stereo_output_mode_t *stereo_mode_ddplus = (ismd_audio_ddplus_stereo_output_mode_t *) &param;
  *stereo_mode_ddplus = ISMD_AUDIO_DDPLUS_STEREO_OUTPUT_MODE_SURROUND_COMPATIBLE;
  CLog::Log(LOGNONE, "ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_STEREO_OUTPUT_MODE_SURROUND_COMPATIBLE %d", *stereo_mode_ddplus);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_DDPLUS_STEREO_OUTPUT_MODE, &param);

  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_STEREO_OUTPUT_MODE_SURROUND_COMPATIBLE failed %d", result);
  }

  ismd_audio_ddplus_output_configuration_t *output_config_ddplus = (ismd_audio_ddplus_output_configuration_t *) &param;
  *output_config_ddplus = (ismd_audio_ddplus_output_configuration_t)g_guiSettings.GetInt("audiooutput.ddplus_output_config");
  if(bAC3Encode) // if the output will be rencode to AC3, output all 5 channels
    *output_config_ddplus = ISMD_AUDIO_DDPLUS_OUTPUT_CONFIGURATION_3_2_1_LCRlrTs;
  CLog::Log(LOGNONE, "ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_OUTPUT_CONFIGURATION %d", *output_config_ddplus);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle, ISMD_AUDIO_DDPLUS_OUTPUT_CONFIGURATION, &param);

  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyPlusModes ISMD_AUDIO_DDPLUS_OUTPUT_CONFIGURATION failed %d", result);
  }
}

void CIntelSMDAudioRenderer::ConfigureDolbyTrueHDModes(ismd_audio_processor_t proc_handle, ismd_dev_t input_handle)
{
  ismd_result_t result;
  ismd_audio_decoder_param_t param;

  int drcmode = g_guiSettings.GetInt("audiooutput.dd_truehd_drc");
  int drc_percent = g_guiSettings.GetInt("audiooutput.dd_truehd_drc_percentage");

  //Dolby TrueHD DRC is on by default. We require the ability to set it off and auto mode.
  // This command sets the DRC mode: 0 off, 1 auto (default), 2 forced.
  ismd_audio_truehd_drc_enable_t *drc_control =
      (ismd_audio_truehd_drc_enable_t *) &param;

  if (drcmode == DD_TRUEHD_DRC_AUTO)
    *drc_control = 1; // Set to auto
  else if (drcmode == DD_TRUEHD_DRC_OFF)
    *drc_control = 0;
  else if (drcmode == DD_TRUEHD_DRC_ON)
    *drc_control = 2;
  CLog::Log(LOGNONE, "ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DRC_ENABLE %d", *drc_control);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
      ISMD_AUDIO_TRUEHD_DRC_ENABLE, &param);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DRC_ENABLE failed %d", result);
  }

  if (drcmode == DD_TRUEHD_DRC_ON)
  {
    ismd_audio_truehd_drc_boost_t *dobly_drc_percent = (ismd_audio_truehd_drc_enable_t *) &param;
    *dobly_drc_percent = drc_percent;

    CLog::Log(LOGNONE, "ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DRC_BOOST %d", *dobly_drc_percent);
    result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_TRUEHD_DRC_BOOST, &param);
    if (result != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DRC_BOOST failed %d", result);
    }
  }

  //Dolby TrueHD 2 channels presentation decoding. We need to decode this properly.
  //Instead we decode the multi-channel presentation and downmix to 2 channels.

  /* This command sets the decode_mode and configures the decoder as a two-, six- or eight-channel decoder. Valid values are 2, 6, and 8 (default)*/
  ismd_audio_truehd_decode_mode_t *decode_control =
      (ismd_audio_truehd_decode_mode_t *) &param;
  *decode_control = 2; // Set to 2 channel
  CLog::Log(LOGNONE, "ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DECODE_MODE %d", *decode_control);
  result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
      ISMD_AUDIO_TRUEHD_DECODE_MODE, &param);
  if (result != ISMD_SUCCESS)
  {
    CLog::Log(LOGERROR, "CIntelSMDGlobals::ConfigureDolbyTrueHDModes ISMD_AUDIO_TRUEHD_DECODE_MODE failed %d", result);
  }

}

void CIntelSMDAudioRenderer::ConfigureDTSModes(
    ismd_audio_processor_t proc_handle, ismd_dev_t input_handle)
{
  ismd_result_t result;
  ismd_audio_decoder_param_t dec_param;


  //please set downmix_mode as ISMD_AUDIO_DTS_DOWNMIX_MODE_INTERNAL
  /* Valid values: "ISMD_AUDIO_DTS_DOWNMIX_MODE_EXTERNAL" | "ISMD_AUDIO_DTS_DOWNMIX_MODE_INTERNAL" */
  {
    /* Set the downmix mode parameter for DTS core decoder. */
    ismd_audio_dts_downmix_mode_t *param = (ismd_audio_dts_downmix_mode_t *) &dec_param;
    *param = ISMD_AUDIO_DTS_DOWNMIX_MODE_INTERNAL;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_DOWNMIXMODE %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_DOWNMIXMODE, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "Setting the downmix mode on DTS CORE decoder failed!", result);
    }
  }

  //please set speaker_out as 2   /* Please see ismd_audio_dts for acceptable values.(0 = default spkr out)*/
  {
    /* Set the Speaker output configuration of the DTS core decoder. */
    ismd_audio_dts_speaker_output_configuration_t *param = (ismd_audio_dts_speaker_output_configuration_t *) &dec_param;
    *param = 2;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_SPKROUT %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_SPKROUT, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "Setting the speaker output mode on DTS CORE decoder failed!",
          result);
    }
  }
  // please set dynamic_range_percent as 0  /* Valid values: 0 - 100*/
  {
    /* Set the DRC percent of the DTS core decoder. */
    ismd_audio_dts_dynamic_range_compression_percent_t *param = (ismd_audio_dts_dynamic_range_compression_percent_t *) &dec_param;
    *param = 0;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_DRCPERCENT %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_DRCPERCENT, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,"Setting the DRC percent on DTS CORE decoder failed!", result);
    }
  }

  // please set number_output_channels as 2  /* Valid values:  Specify number of output channels 1 - 6.*/
  {
    /* Set the number of output channels of the DTS core decoder. */
    ismd_audio_dts_num_output_channels_t *param = (ismd_audio_dts_num_output_channels_t *) &dec_param;
    *param = 2;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_NUM_OUTPUT_CHANNELS %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_NUM_OUTPUT_CHANNELS, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,"Setting the num output channels on DTS CORE decoder failed!",
          result);
    }
  }
  // please set lfe_downmix as ISMD_AUDIO_DTS_LFE_DOWNMIX_NONE
  /* Valid values: "ISMD_AUDIO_DTS_LFE_DOWNMIX_NONE" | "ISMD_AUDIO_DTS_LFE_DOWNMIX_10DB" */
  {
    /* Set the LFE downmix setting of the DTS core decoder. */
    int *param = (int *) &dec_param;
    *param = ISMD_AUDIO_DTS_LFE_DOWNMIX_NONE;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_LFEDNMX %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_LFEDNMX, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR,"Setting the LFE downmix on DTS CORE decoder failed!", result);
    }
  }

  // please set sec_audio_scale as 0      /* Valid values: 0 | 1 */
  {
    /* Set the LFE downmix setting of the DTS core decoder. */
    ismd_audio_dts_secondary_audio_scale_t *param = (ismd_audio_dts_secondary_audio_scale_t *) &dec_param;
    *param = 0;
    CLog::Log(LOGNONE, "ConfigureDTSModes ISMD_AUDIO_DTS_SEC_AUD_SCALING %d", *param);
    if ((result = ismd_audio_input_set_decoder_param(proc_handle, input_handle,
        ISMD_AUDIO_DTS_SEC_AUD_SCALING, &dec_param)) != ISMD_SUCCESS)
    {
      CLog::Log(LOGERROR, "Setting the secondary audio scaling on DTS CORE decoder failed!", result);
    }
  }
}


#endif
