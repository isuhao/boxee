#include "GUIDialogBoxeeOTAFacebookConnect.h"
#include "GUIWindowManager.h"
#include "BoxeeOTAConfigurationManager.h"
#include "GUIDialogBoxeeOTALocationConfiguration.h"
#include "utils/log.h"
#include "Application.h"

#define FACEBOOK_BUTTON 1

///////////////////////////////////////////////////////////////////////////////////////////////////////

CGUIDialogBoxeeOTAFacebookConnect::CGUIDialogBoxeeOTAFacebookConnect()
  : CGUIDialogBoxeeWizardBase(WINDOW_OTA_FACEBOOK_CONNECT, "custom_boxee_livetv_setup_5a.xml", "CGUIDialogBoxeeOTAFacebookConnect")
{
}

CGUIDialogBoxeeOTAFacebookConnect::~CGUIDialogBoxeeOTAFacebookConnect()
{
}

void CGUIDialogBoxeeOTAFacebookConnect::OnInitWindow()
{
  return CGUIDialogBoxeeWizardBase::OnInitWindow();
}

void CGUIDialogBoxeeOTAFacebookConnect::OnDeinitWindow(int nextWindowID)
{
  CGUIWindow::OnDeinitWindow(nextWindowID);
}

bool CGUIDialogBoxeeOTAFacebookConnect::OnAction(const CAction& action)
{
  if (action.id == ACTION_SELECT_ITEM)
  {
    int iControl = GetFocusedControlID();

    if(iControl == DIALOG_WIZARD_BUTTON_NEXT)
    {
      g_application.GetBoxeeSocialUtilsUIManager().HandleUISocialUtilConnect(FACEBOOK_SERVICE_ID);
    }

    m_actionChoseEnum = CActionChoose::NEXT;
    Close();
    return true;
  }

  return CGUIDialogBoxeeWizardBase::OnAction(action);
}
