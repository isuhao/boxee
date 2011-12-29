/* CGUIDialogBoxeeOTAConfiguration .cpp
 *
 */
#include "GUIDialogBoxeeOTAConfiguration.h"
#include "GUIWindowManager.h"
#include "BoxeeOTAConfigurationManager.h"
#include "GUIDialogBoxeeOTALocationConfiguration.h"
#include "utils/log.h"

///////////////////////////////////////////////////////////////////////////////////////////////////////

CGUIDialogBoxeeOTAConnectionConfiguration::CGUIDialogBoxeeOTAConnectionConfiguration()
  : CGUIDialogBoxeeWizardBase(WINDOW_OTA_CONNECTION_CONFIGURATION,"custom_boxee_livetv_setup_4a.xml","CGUIDialogBoxeeOTAConnectionConfiguration")
{
}

CGUIDialogBoxeeOTAConnectionConfiguration::~CGUIDialogBoxeeOTAConnectionConfiguration()
{
}

bool CGUIDialogBoxeeOTAConnectionConfiguration::OnAction(const CAction& action)
{
  if (action.id == ACTION_SELECT_ITEM)
  {
    int iControl = GetFocusedControlID();

    if(iControl == DIALOG_WIZARD_BUTTON_NEXT)
    {
      CBoxeeOTAConfigurationManager::GetInstance().GetConfigurationData().SetIsCable(false);
    }
    if(iControl == DIALOG_WIZARD_BUTTON_BACK)
    {
      CBoxeeOTAConfigurationManager::GetInstance().GetConfigurationData().SetIsCable(true);
    }
    m_actionChoseEnum = CActionChoose::NEXT;
    Close();
    return true;
  }
  return CGUIDialogBoxeeWizardBase::OnAction(action);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
CGUIDialogBoxeeOTAWelcome::CGUIDialogBoxeeOTAWelcome() : CGUIDialogBoxeeWizardBase(WINDOW_OTA_WELCOME_CONFIGURATION, "custom_boxee_livetv_setup_1.xml", "CGUIDialogBoxeeOTAConfiguration")
{
}

CGUIDialogBoxeeOTAWelcome::~CGUIDialogBoxeeOTAWelcome()
{
}
