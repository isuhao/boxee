#ifndef GUIDIALOGBOXEELIVETVEDITCHANNELS_H
#define GUIDIALOGBOXEELIVETVEDITCHANNELS_H

#include "system.h"

#ifdef HAS_DVB

//#include "LiveTvModel.h"
#include "GUIDialog.h"
//#include "FileItem.h"

#include <set>

class CGUIDialogBoxeeLiveTvEditChannels : public CGUIDialog
{
public:

  CGUIDialogBoxeeLiveTvEditChannels();
  virtual ~CGUIDialogBoxeeLiveTvEditChannels();
  virtual bool OnMessage(CGUIMessage& message);

protected:
  virtual void OnInitWindow();
  virtual void OnDeinitWindow(int nextWindowID);

private:
  bool LoadChannelsList();
  bool HandleClickedChannel();

  bool m_dirty;
};


#endif

#endif // GUIDIALOGBOXEELIVETVEDITCHANNELS_H

