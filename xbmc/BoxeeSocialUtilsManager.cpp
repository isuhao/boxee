
#include "BoxeeSocialUtilsManager.h"
#include "utils/log.h"
#include "BoxeeUtils.h"

using namespace BOXEE;
using namespace DIRECTORY;

CBoxeeSocialUtilsManager::CBoxeeSocialUtilsManager()
{
  m_bIsInit = false;
}

CBoxeeSocialUtilsManager::~CBoxeeSocialUtilsManager()
{
}

bool CBoxeeSocialUtilsManager::InitBoxeeSocialUtilsManager()
{
  if(m_bIsInit)
  {
    CLog::Log(LOGWARNING,"CBoxeeSocialUtilsManager::InitBoxeeSocialUtilsManager - reset BoxeeSocialUtilsManager didn't occur");
  }
  ResetBoxeeSocialUtilsManager();

  if (!GetSocialServicesStatus())
  {
    CLog::Log(LOGERROR,"CBoxeeSocialUtilsManager::InitBoxeeSocialUtilsManager() - FAILED to get social services status from server.");
    return false;
  }
  m_bIsInit = true;
  return true;
}

void CBoxeeSocialUtilsManager::ResetBoxeeSocialUtilsManager()
{
  m_isFacebookConnected = false;
  m_isTwitterConnected = false;
  m_isTumblrConnected = false;
  m_bIsInit = false;

  m_serviceToLinkMap.clear();
  m_serviceToExternalLinkMap.clear();
}

bool CBoxeeSocialUtilsManager::IsBoxeeSocialUtilsMangerInit()
{
  return m_bIsInit;
}

CStdString CBoxeeSocialUtilsManager::GetServiceLink(const CStdString& serviceId)
{
#ifdef CANMORE
  return m_serviceToLinkMap[serviceId];
#else
  return m_serviceToExternalLinkMap[serviceId];
#endif
}

bool CBoxeeSocialUtilsManager::GetSocialServicesStatus(const CStdString& _serviceId)
{
  int retCode;
  Job_Result jobResult = BoxeeUtils::GetShareServicesJson(m_jsonServiceList, retCode);

  CLog::Log(LOGDEBUG,"CBoxeeSocialUtilsManager::GetSocialServicesStatus - call to get SocialServices status from server returned [jobResult=%d] ",jobResult);

  if (jobResult != JOB_SUCCEEDED)
  {
    CLog::Log(LOGERROR,"CBoxeeSocialUtilsManager::GetSocialServicesStatus - FAILED to get SocialServices status from server. [jobResult=%d]",jobResult);
    return false;
  }

  m_servicesList.Clear();
  m_serviceToLinkMap.clear();
  m_serviceToExternalLinkMap.clear();

  BoxeeUtils::ParseJsonShareServicesToFileItems(m_jsonServiceList,m_servicesList);

  CLog::Log(LOGDEBUG,"CBoxeeSocialUtilsManager::GetSocialServicesStatus - after parse SocialServices to FIleItemList. [NumOfSocialServices=%d]",m_servicesList.Size());

  for (int i=0; i<m_servicesList.Size(); i++)
  {
    CFileItemPtr item = m_servicesList.Get(i);

    CStdString serviceId = item->GetProperty("serviceId");

    if (serviceId == FACEBOOK_SERVICE_ID)
    {
      m_isFacebookConnected = item->GetPropertyBOOL("enable");
      m_serviceToLinkMap[FACEBOOK_SERVICE_ID] = item->GetProperty("connect");
      m_serviceToExternalLinkMap[FACEBOOK_SERVICE_ID] = item->GetProperty("link");

      if(_serviceId == FACEBOOK_SERVICE_ID)
        break;
    }
    else if (serviceId == TWITTER_SERVICE_ID)
    {
      m_isTwitterConnected = item->GetPropertyBOOL("enable");
      m_serviceToLinkMap[TWITTER_SERVICE_ID] = item->GetProperty("connect");
      m_serviceToExternalLinkMap[TWITTER_SERVICE_ID] = item->GetProperty("link");

      if(_serviceId == TWITTER_SERVICE_ID)
        break;
    }
    else if (serviceId == TUMBLR_SERVICE_ID)
    {
      m_isTumblrConnected = item->GetPropertyBOOL("enable");
      m_serviceToLinkMap[TUMBLR_SERVICE_ID] = item->GetProperty("connect");
      m_serviceToExternalLinkMap[TUMBLR_SERVICE_ID] = item->GetProperty("link");

      if(_serviceId == TUMBLR_SERVICE_ID)
        break;
    }
  }

  return true;
}

bool CBoxeeSocialUtilsManager::GetIsSocialUtilConnected(const CStdString& serviceId, bool getDataFromServer)
{
  if(getDataFromServer)
  {
    GetSocialServicesStatus(serviceId);
  }
  return m_isFacebookConnected;
}


void CBoxeeSocialUtilsManager::SetIsSocialUtilConnected(const CStdString& serviceId,bool isConnected)
{
  if(serviceId == FACEBOOK_SERVICE_ID)
  {
    m_isFacebookConnected = isConnected;
  }
  if(serviceId == TWITTER_SERVICE_ID)
  {
    m_isTwitterConnected = isConnected;
  }
  if(serviceId == TUMBLR_SERVICE_ID)
  {
    m_isTumblrConnected = isConnected;
  }
}

