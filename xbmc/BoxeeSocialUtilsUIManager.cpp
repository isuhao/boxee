
#include "BoxeeSocialUtilsUIManager.h"
#include "utils/log.h"
#include "BoxeeUtils.h"
#include "GUIWebDialog.h"
#include "Application.h"
#include "GUIDialogOK2.h"
#include "LocalizeStrings.h"

using namespace BOXEE;
using namespace DIRECTORY;

CBoxeeSocialUtilsUIManager::CBoxeeSocialUtilsUIManager()
{
}

CBoxeeSocialUtilsUIManager::~CBoxeeSocialUtilsUIManager()
{
}

bool CBoxeeSocialUtilsUIManager::ConnectUISocialService(const CStdString& serviceId)
{
  CStdString url =  g_application.GetBoxeeSocialUtilsManager().GetServiceLink(serviceId);
#ifdef CANMORE
  if(url == "")
  {
    CLog::Log(LOGERROR,"CBoxeeSocialUtilsUIManager::ConnectUISocialService - url is empty return FALSE");
  }
  CLog::Log(LOGDEBUG,"CBoxeeSocialUtilsUIManager::ConnectUISocialService - open GUIWebDialog with [url=%s]",url.c_str());

  return CGUIWebDialog::ShowAndGetInput(url);
#else
  CStdString text = g_localizeStrings.Get(80001);
  text += " " + url;
  CGUIDialogOK2::ShowAndGetInput(g_localizeStrings.Get(10014),text);
  return false;
#endif

}

bool CBoxeeSocialUtilsUIManager::HandleUISocialUtilConnect(const CStdString& serviceId)
{
  if (g_application.GetBoxeeSocialUtilsManager().GetIsSocialUtilConnected(serviceId,true))
  {
    CLog::Log(LOGDEBUG,"CBoxeeSocialUtilsUIManager::HandleUISocialUtilConnect - [%s] is already enable ",serviceId.c_str());
    return true;
  }

  if (!ConnectUISocialService(serviceId))
  {
    CLog::Log(LOGERROR,"CBoxeeSocialUtilsUIManager::HandleUISocialUtilConnect - FAILED to connect [%s]",serviceId.c_str());
    return false;
  }

  return true;
}
