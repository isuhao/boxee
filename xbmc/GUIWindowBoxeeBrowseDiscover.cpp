
#include "GUIWindowBoxeeBrowseDiscover.h"
#include "FileSystem/BoxeeServerDirectory.h"
#include "GUIWindowStateDatabase.h"
#include "GUIWindowManager.h"
#include "lib/libBoxee/bxconfiguration.h"
#include "URL.h"
#include "Util.h"
#include "BoxeeUtils.h"
#include "utils/log.h"
#include "LocalizeStrings.h"
#include "GUIUserMessages.h"
#include "GUISettings.h"
#include "DirectoryCache.h"
#include "Application.h"

using namespace std;
using namespace BOXEE;

#define THUMB_VIEW_LIST       50
#define LINE_VIEW_LIST        51

#define FACEBOOK_BUTTON      250
#define TWITTER_BUTTON       251
#define TUMBLR_BUTTON        252
#define DONE_BUTTON          7092
#define EMPTY_STATE_LABEL    7094


CDiscoverWindowState::CDiscoverWindowState(CGUIWindowBoxeeBrowse* pWindow) : CBrowseWindowState(pWindow)
{
  m_sourceController.SetNewSource(new CBrowseWindowSource("discoversource", "feed://share", pWindow->GetID()));
}

void CDiscoverWindowState::SetDefaultView()
{
  m_iCurrentView = THUMB_VIEW_LIST;
}

CGUIWindowBoxeeBrowseDiscover::CGUIWindowBoxeeBrowseDiscover() : CGUIWindowBoxeeBrowse(WINDOW_BOXEE_BROWSE_DISCOVER, "boxee_browse_discover.xml")
{
  SetWindowState(new CDiscoverWindowState(this));

  m_hasBrowseMenu = false;
}


CGUIWindowBoxeeBrowseDiscover::~CGUIWindowBoxeeBrowseDiscover()
{
  
}

void CGUIWindowBoxeeBrowseDiscover::OnInitWindow()
{
  CGUIWindowBoxeeBrowse::OnInitWindow();
}

void CGUIWindowBoxeeBrowseDiscover::OnDeinitWindow(int nextWindowID)
{
  CGUIWindowBoxeeBrowse::OnDeinitWindow(nextWindowID);
}

bool CGUIWindowBoxeeBrowseDiscover::HandleEmptyState()
{
  bool isEmpty = false;

  if (m_vecViewItems.Size() == 0)
  {
    isEmpty = true;

    CGUIWindowStateDatabase stateDB;
    CStdString isFirstTimeUser;
    bool settingExists = stateDB.GetUserSetting("firstTimeFriendsRequest", isFirstTimeUser);
    if(!settingExists)
    {
      stateDB.SetUserSetting("firstTimeFriendsRequest","true");
      bool isConnectedToFacebook = g_application.GetBoxeeSocialUtilsManager().GetIsSocialUtilConnected(FACEBOOK_SERVICE_ID);
      bool isConnectedToTwitter = g_application.GetBoxeeSocialUtilsManager().GetIsSocialUtilConnected(TWITTER_SERVICE_ID);
      bool isConnectedToTumblr = g_application.GetBoxeeSocialUtilsManager().GetIsSocialUtilConnected(TUMBLR_SERVICE_ID);

      if(!isConnectedToFacebook && !isConnectedToTwitter && !isConnectedToTumblr)
      {
        SetProperty("emptyFirstTime", isEmpty);
        SetProperty("empty", false);
        SET_CONTROL_FOCUS(DONE_BUTTON, 0);
        return isEmpty;
      }
    }

    SetProperty("empty", isEmpty);
    SetProperty("emptyFirstTime", false);
    CONTROL_DISABLE(DONE_BUTTON);
    SET_CONTROL_HIDDEN(DONE_BUTTON);
    SET_CONTROL_FOCUS(EMPTY_STATE_LABEL,0);
  }
  else
  {
    SetProperty("empty",isEmpty);
    SetProperty("emptyFirstTime", isEmpty);
  }

  return isEmpty;
}

bool CGUIWindowBoxeeBrowseDiscover::OnAction(const CAction& action)
{
  if (action.id == ACTION_PREVIOUS_MENU || action.id == ACTION_PARENT_DIR)
  {
    OnBack();
    return true;
  }
  return CGUIWindowBoxeeBrowse::OnAction(action);
}

bool CGUIWindowBoxeeBrowseDiscover::OnMessage(CGUIMessage& message)
{
  if (message.GetMessage() == GUI_MSG_UPDATE)
  {
    LOG(LOG_LEVEL_DEBUG,"CGUIWindowBoxeeBrowseDiscover::OnMessage - GUI_MSG_UPDATE - Enter function with [SenderId=%d] (rec)(browse)",message.GetSenderId());

    if (message.GetSenderId() != GetID())
    {
      return true;
    }

    bool clearCache = message.GetParam1();

    LOG(LOG_LEVEL_DEBUG,"CGUIWindowBoxeeBrowseDiscover::OnMessage - GUI_MSG_UPDATE - Enter function with [clearCache=%d] (rec)(browse)",clearCache);

    if (clearCache)
    {
      g_directoryCache.ClearSubPaths("feed://share/");
      g_directoryCache.ClearSubPaths("feed://recommend/");
    }
  }
  if(message.GetMessage() == GUI_MSG_CLICKED)
  {
    if(message.GetSenderId() == FACEBOOK_BUTTON)
    {
      g_application.GetBoxeeSocialUtilsUIManager().HandleUISocialUtilConnect(FACEBOOK_SERVICE_ID);
      return true;
    }
    if(message.GetSenderId() == TWITTER_BUTTON)
    {
      g_application.GetBoxeeSocialUtilsUIManager().HandleUISocialUtilConnect(TWITTER_SERVICE_ID);
      return true;
    }
    if(message.GetSenderId() == TUMBLR_BUTTON)
    {
      g_application.GetBoxeeSocialUtilsUIManager().HandleUISocialUtilConnect(TUMBLR_SERVICE_ID);
      return true;
    }
    if(message.GetSenderId() == DONE_BUTTON)
    {
      HandleEmptyState();
      return true;
    }
  }

  return CGUIWindowBoxeeBrowse::OnMessage(message);
}

