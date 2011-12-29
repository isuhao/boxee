#include <string>

#include "LicenseConfig.h"

#ifdef HAS_EMBEDDED

#include "lib/libBoxee/bxoemconfiguration.h"

const int DEF_MAX_PCM_CHANNELS_DECODE = -1;
const int DEF_MAX_CHANNELS_ENCODE = -1;
const VENDOR DEF_PREFERRED_ENCODING = AUDIO_VENDOR_NONE;
const int DEF_IS_DOLBY_SW_DECODE_ALLOWED = 0;
const int DEF_IS_DTS_SW_DECODE_ALLOWED = 0;

void CLicenseSettings::Load()
{
  CVendorLicense free_license = {-1, -1};
  m_vendor_licenses[AUDIO_VENDOR_NONE] = free_license;
    CVendorLicense dolby_license = {
        BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.Dolby.MaxPCMChannelsDecode",DEF_MAX_PCM_CHANNELS_DECODE),\
        BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.Dolby.MaxChannelsEncode",DEF_MAX_CHANNELS_ENCODE)};
    m_vendor_licenses[AUDIO_VENDOR_DOLBY] = dolby_license;
   CVendorLicense dts_license = {
        BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.DTS.MaxPCMChannelsDecode",DEF_MAX_PCM_CHANNELS_DECODE),\
        BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.DTS.MaxChannelsEncode",DEF_MAX_CHANNELS_ENCODE)};
    m_vendor_licenses[AUDIO_VENDOR_DTS] = dts_license;

    m_is_dolby_sw_decode_allowed = (bool) BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.Dolby.SoftwareDecoder", DEF_IS_DOLBY_SW_DECODE_ALLOWED);
    m_is_dts_sw_decode_allowed = (bool) BOXEE::BXOEMConfiguration::GetInstance().GetIntParam("Boxee.Audio.DTS.SoftwareDecoder", DEF_IS_DTS_SW_DECODE_ALLOWED);

   std::string preferred_encoding = BOXEE::BXOEMConfiguration::GetInstance().GetStringParam("Boxee.Audio.PreferredEncoding");
   if(preferred_encoding == "DOLBY")
       m_preferred_encoding = AUDIO_VENDOR_DOLBY;
   else if(preferred_encoding == "DTS")
       m_preferred_encoding = AUDIO_VENDOR_DTS;
   else
   {
       if(preferred_encoding != "NONE")
           CLog::Log(LOGERROR, "Invalid preferred encoding value: %s", preferred_encoding.c_str());
       m_preferred_encoding = AUDIO_VENDOR_NONE;
   }
   m_is_initialized = true;
}

int CLicenseSettings::get_max_channels_decode(VENDOR vendor) const
{
    if(!m_is_initialized)
    {
         CLog::Log(LOGERROR, "Must call CLicenseSettings::Load() before using the CLicenseSettings class");
         return DEF_MAX_CHANNELS_ENCODE;
    }
#ifdef HAS_EMBEDDED
    return m_vendor_licenses.at(vendor).m_max_channels_decode;
#else
    std::map<VENDOR, CVendorLicense>::const_iterator it = m_vendor_licenses.find(vendor);
    if (it != m_vendor_licenses.end())
    {
      return (it->second).m_max_channels_decode;
    }

    return DEF_MAX_CHANNELS_ENCODE;
#endif
}

int CLicenseSettings::get_max_channels_encode(VENDOR vendor) const
{
    if(!m_is_initialized)
    {
         CLog::Log(LOGERROR, "Must call CLicenseSettings::Load() before using the CLicenseSettings class");
         return DEF_MAX_PCM_CHANNELS_DECODE;
    }
#ifdef HAS_EMBEDDED
    return m_vendor_licenses.at(vendor).m_max_channels_encode;
#else
    std::map<VENDOR, CVendorLicense>::const_iterator it = m_vendor_licenses.find(vendor);
    if (it != m_vendor_licenses.end())
    {
      return (it->second).m_max_channels_encode;
    }

    return DEF_MAX_PCM_CHANNELS_DECODE;
#endif
}

VENDOR CLicenseSettings::get_preferred_encoding() const
{
    if(!m_is_initialized)
    {
         CLog::Log(LOGERROR, "Must call CLicenseSettings::Load() before using the CLicenseSettings class");
         return DEF_PREFERRED_ENCODING;
    }
    return m_preferred_encoding;
}

bool CLicenseSettings::is_dolby_sw_decode_allowed() const
{
    if(!m_is_initialized)
    {
         CLog::Log(LOGERROR, "Must call CLicenseSettings::Load() before using the CLicenseSettings class");
         return false;
    }
    return m_is_dolby_sw_decode_allowed;
}

bool CLicenseSettings::is_dts_sw_decode_allowed() const
{
    if(!m_is_initialized)
    {
         CLog::Log(LOGERROR, "Must call CLicenseSettings::Load() before using the CLicenseSettings class");
         return false;
    }
    return m_is_dts_sw_decode_allowed;
}

CLicenseSettings g_lic_settings;
#endif
