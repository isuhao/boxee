
#ifndef BOXEESOCIALUTILSMANAGER_H
#define BOXEESOCIALUTILSMANAGER_H

#include <string>
#include "GUIWindow.h"
#include "FileItem.h"
#include "lib/libjson/include/json/value.h"

#define FACEBOOK_SERVICE_ID                   "311"
#define TWITTER_SERVICE_ID                    "413"
#define TUMBLR_SERVICE_ID                     "323"

class CBoxeeSocialUtilsManager{
public:
  CBoxeeSocialUtilsManager();
  virtual ~CBoxeeSocialUtilsManager();
  bool InitBoxeeSocialUtilsManager();
  void ResetBoxeeSocialUtilsManager();
  bool IsBoxeeSocialUtilsMangerInit();
  CStdString GetServiceLink(const CStdString& serviceId);

  bool GetIsSocialUtilConnected(const CStdString& serviceId, bool getDataFromServer = false);

  void SetIsSocialUtilConnected(const CStdString& serviceId,bool isConnected);

private:
  bool GetSocialServicesStatus(const CStdString& _serviceId = "");
  CFileItemList m_servicesList;
  Json::Value m_jsonServiceList;

  std::map<CStdString,CStdString> m_serviceToLinkMap;
  std::map<CStdString,CStdString> m_serviceToExternalLinkMap;

  bool m_isFacebookConnected;
  bool  m_isTwitterConnected;
  bool  m_isTumblrConnected;
  bool m_bIsInit;
};

#endif
