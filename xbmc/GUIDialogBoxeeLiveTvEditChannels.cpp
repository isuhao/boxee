#include "GUIDialogBoxeeLiveTvEditChannels.h"

#ifdef HAS_DVB
#include "Util.h"
#include <boost/foreach.hpp>
#include "Application.h"
#include "GUIWindowManager.h"
#include "cores/dvb/dvbmanager.h"
#include "utils/log.h"
#include "GUIListContainer.h"
#include "LiveTvModel.h"
#include "FileItem.h"
#include "GUIDialogProgress.h"

using namespace BOXEE;

#define CONTROL_LIST_CHANNELS            46

CGUIDialogBoxeeLiveTvEditChannels::CGUIDialogBoxeeLiveTvEditChannels() : CGUIDialog(WINDOW_DIALOG_BOXEE_LIVETV_EDIT_CHANNELS, "boxee_livetv_edit_channels.xml")
{
}

CGUIDialogBoxeeLiveTvEditChannels::~CGUIDialogBoxeeLiveTvEditChannels()
{
}

void CGUIDialogBoxeeLiveTvEditChannels::OnInitWindow()
{
  CGUIDialog::OnInitWindow();

  if (!LoadChannelsList())
  {
    CLog::Log(LOGERROR,"CGUIDialogBoxeeLiveTvEditChannels::OnInitWindow - FAILED to load channels (ltvc)");
    Close();
    return;
  }

  m_dirty = false;
}

void CGUIDialogBoxeeLiveTvEditChannels::OnDeinitWindow(int nextWindowID)
{
  if (m_dirty)
  {
#ifdef HAS_SERVER_OTA
    CGUIDialogProgress* progress = (CGUIDialogProgress *)g_windowManager.GetWindow(WINDOW_DIALOG_PROGRESS);
    progress->StartModal();
    progress->Progress();

    if (!DVBManager::GetInstance().GetChannels().SaveChannelsToServer())
    {
      CLog::Log(LOGERROR,"CGUIDialogBoxeeLiveTvEditChannels::OnDeinitWindow - failed to save channels to server");
    }

    progress->Close();
#endif
  }

  CGUIDialog::OnDeinitWindow(nextWindowID);
}

bool CGUIDialogBoxeeLiveTvEditChannels::OnMessage(CGUIMessage& message)
{
  switch (message.GetMessage())
  {
  case GUI_MSG_LABEL_BIND:
  {
    if (message.GetPointer() && message.GetControlId() == 0)
    {
      CFileItemList* items = (CFileItemList*)message.GetPointer();
      delete items;
      return true;
    }
  }
  break;

  case GUI_MSG_CLICKED:
  {
    if (message.GetSenderId() == CONTROL_LIST_CHANNELS)
    {
      return HandleClickedChannel();
    }
  }
  break;

  default:
  break;
  }

  return CGUIDialog::OnMessage(message);
}

bool CGUIDialogBoxeeLiveTvEditChannels::LoadChannelsList()
{
  LiveTvModel model;
  LiveTvModelChannelsType channels = model.GetChannels();

  CFileItemList m_channelsList;

  int currentChannelIndex = DVBManager::GetInstance().GetCurrentChannel()->GetIndex();
  int selectedItem = 0;

  foreach (LiveTvModelChannel channel, channels)
  {
    CStdString label = channel.number;
    if (!label.IsEmpty())
        label += " ";
    label += channel.label;

    CFileItemPtr channelItem(new CFileItem(label));
    channelItem->SetProperty("ChannelName", channel.label);
    channelItem->SetProperty("ChannelEnabled", channel.enabled);
    channelItem->SetProperty("channel-id", channel.id);

    if (currentChannelIndex == channel.id)
      selectedItem = m_channelsList.Size();

    m_channelsList.Add(channelItem);
  }

  CGUIMessage msgBind(GUI_MSG_LABEL_BIND, GetID(), CONTROL_LIST_CHANNELS, 0,0, &m_channelsList);
  OnMessage(msgBind);

  SET_CONTROL_FOCUS(CONTROL_LIST_CHANNELS, selectedItem);

  return true;
}

bool CGUIDialogBoxeeLiveTvEditChannels::HandleClickedChannel()
{
  CGUIBaseContainer* pContainer = (CGUIBaseContainer*)GetControl(CONTROL_LIST_CHANNELS);
  if (!pContainer)
  {
    CLog::Log(LOGERROR,"CGUIDialogBoxeeLiveTvEditChannels::HandleClickedChannel - FAILED to get CONTROL_LIST_CHANNELS object (ltvc)");
    return true;
  }

  CGUIListItemPtr clickedChannel = pContainer->GetSelectedItemPtr();
  clickedChannel->SetProperty("ChannelEnabled", !clickedChannel->GetPropertyBOOL("ChannelEnabled"));

  DvbChannelPtr channel = DVBManager::GetInstance().GetChannels().GetChannelByIndex(clickedChannel->GetPropertyInt("channel-id"));
  channel->SetEnabled(!channel->IsEnabled());

  m_dirty = true;

  return true;
}

#endif

