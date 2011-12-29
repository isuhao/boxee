#pragma once

#include "system.h"

#ifdef HAS_DVB

#include "GUIDialog.h"

class CGUIDialogBoxeeLiveTvScan : public CGUIDialog
{
public:
  CGUIDialogBoxeeLiveTvScan();
  virtual ~CGUIDialogBoxeeLiveTvScan();
  virtual void OnInitWindow();
  virtual bool OnAction(const CAction& action);
  virtual bool OnMessage(CGUIMessage& message);
  virtual void Render();

  bool RequestedGotToLiveTv() { return m_requestGoToLiveTv; }

private:
  void HideAll();
  void StartScan();
  void CancelScan();
  void ShowScanning();
  void ShowNoChannels();
  void ShowNoInternet();
  void ShowDone();
  void ConfirmCancel();

  bool m_requestGoToLiveTv;
  bool m_scanStarted;
};

#endif
