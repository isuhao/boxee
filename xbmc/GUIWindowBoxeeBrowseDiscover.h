#ifndef GUIWINDOWBOXEEBROWSEDISCOVER_H_
#define GUIWINDOWBOXEEBROWSEDISCOVER_H_

#include "GUIWindowBoxeeBrowse.h"

class CDiscoverWindowState : public CBrowseWindowState
{
public:
  CDiscoverWindowState(CGUIWindowBoxeeBrowse* pWindow);
  void SetDefaultView();
};

class CGUIWindowBoxeeBrowseDiscover : public CGUIWindowBoxeeBrowse
{
public:
  CGUIWindowBoxeeBrowseDiscover();
  virtual ~CGUIWindowBoxeeBrowseDiscover();

  virtual bool HandleEmptyState();
  virtual bool OnMessage(CGUIMessage& message);
  virtual bool OnAction(const CAction& action);
  virtual void OnInitWindow();
  virtual void OnDeinitWindow(int nextWindowID);
};

#endif /*GUIWINDOWBOXEEBROWSEDISCOVER_H_*/
