/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "InfoLoader.h"
#include "LocalizeStrings.h"
#include "JobManager.h"
#include "TimeUtils.h"

CInfoJob::CInfoJob(CInfoLoader *loader)
{
  m_loader = loader;
}

void CInfoJob::DoWork()
{
  if (m_loader)
    m_loader->DoWork();
}

CInfoLoader::CInfoLoader(unsigned int timeToRefresh)
{
  m_refreshTime = 0;
  m_timeToRefresh = timeToRefresh;
  m_busy = false;
}

CInfoLoader::~CInfoLoader()
{
}

void CInfoLoader::OnJobComplete(unsigned int jobID, CJob *job)
{
  m_refreshTime = CTimeUtils::GetFrameTime() + m_timeToRefresh;
  m_busy = false;
}

CStdString CInfoLoader::GetInfo(int info)
{
  // Refresh if need be
  if (m_refreshTime < CTimeUtils::GetFrameTime() && !m_busy)
  { // queue up the job
    m_busy = true;
    CJobManager::GetInstance().AddJob(new CInfoJob(this), this);
  }
  if (m_busy)
  {
    return BusyInfo(info);
  }
  return TranslateInfo(info);
}

CStdString CInfoLoader::BusyInfo(int info) const
{
  return g_localizeStrings.Get(503);
}

CStdString CInfoLoader::TranslateInfo(int info) const
{
  return "";
}

void CInfoLoader::Refresh()
{
  m_refreshTime = CTimeUtils::GetFrameTime();
}

