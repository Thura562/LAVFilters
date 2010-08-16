/*
 *      Copyright (C) 2010 Hendrik Leppkes
 *      http://www.1f0.de
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
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 *  Initial design and concept by Gabest and the MPC-HC Team, copyright under GPLv2
 *  Contributions by Ti-BEN from the XBMC DSPlayer Project, also under GPLv2
 */

#include "stdafx.h"
#include "LAVFSplitter.h"
#include "DSStreamInfo.h"
#include "LAVFOutputPin.h"

#include "lavfutils.h"

#include <string>

// static constructor
CUnknown* WINAPI CLAVFSplitter::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
{
  return new CLAVFSplitter(pUnk, phr);
}

CLAVFSplitter::CLAVFSplitter(LPUNKNOWN pUnk, HRESULT* phr) 
  : CBaseFilter(NAME("lavf dshow source filter"), pUnk, this,  __uuidof(this), phr)
  , m_rtDuration(0), m_rtStart(0), m_rtStop(0), m_rtCurrent(0)
  , m_dRate(1.0)
  , m_avFormat(NULL)
{
  av_register_all();

  if(phr) { *phr = S_OK; }
}

CLAVFSplitter::~CLAVFSplitter()
{
  CAutoLock cAutoLock(this);

  CAMThread::CallWorker(CMD_EXIT);
  CAMThread::Close();

  m_State = State_Stopped;
  DeleteOutputs();
}

STDMETHODIMP CLAVFSplitter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
  CheckPointer(ppv, E_POINTER);

  *ppv = NULL;

  return 
    QI(IFileSourceFilter)
    QI(IMediaSeeking)
    QI(IAMStreamSelect)
    QI2(IAMExtendedSeeking)
    QI(IKeyFrameInfo)
    __super::NonDelegatingQueryInterface(riid, ppv);
}

// CBaseSplitter
int CLAVFSplitter::GetPinCount()
{
  CAutoLock lock(this);

  return (int)m_pPins.size();
}

CBasePin *CLAVFSplitter::GetPin(int n)
{
  CAutoLock lock(this);

  if (n < 0 ||n >= GetPinCount()) return NULL;
  return m_pPins[n];
}

CLAVFOutputPin *CLAVFSplitter::GetOutputPin(DWORD streamId)
{
  CAutoLock lock(&m_csPins);

  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    if ((*it)->GetStreamId() == streamId) {
      return *it;
    }
  }
  return NULL;
}

// IFileSourceFilter
STDMETHODIMP CLAVFSplitter::Load(LPCOLESTR pszFileName, const AM_MEDIA_TYPE * pmt)
{
  CheckPointer(pszFileName, E_POINTER);

  m_fileName = std::wstring(pszFileName);

  HRESULT hr = S_OK;

  if(FAILED(hr = DeleteOutputs()) || FAILED(hr = CreateOutputs()))
  {
    m_fileName = L"";
  }

  return hr;
}

// Get the currently loaded file
STDMETHODIMP CLAVFSplitter::GetCurFile(LPOLESTR *ppszFileName, AM_MEDIA_TYPE *pmt)
{
  CheckPointer(ppszFileName, E_POINTER);

  size_t strlen = m_fileName.length() + 1;
  *ppszFileName = (LPOLESTR)CoTaskMemAlloc(sizeof(wchar_t) * strlen);

  if(!(*ppszFileName))
    return E_OUTOFMEMORY;

  wcsncpy_s(*ppszFileName, strlen, m_fileName.c_str(), _TRUNCATE);
  return S_OK;
}

REFERENCE_TIME CLAVFSplitter::GetStreamLength()
{
  int64_t iLength = 0;
  if (m_avFormat->duration == (int64_t)AV_NOPTS_VALUE || m_avFormat->duration < 0LL) {
    // no duration is available for us
    // try to calculate it
    // TODO
    /*if (m_rtCurrent != Packet::INVALID_TIME && m_avFormat->file_size > 0 && m_avFormat->pb && m_avFormat->pb->pos > 0) {
    iLength = (((m_rtCurrent * m_avFormat->file_size) / m_avFormat->pb->pos) / 1000) & 0xFFFFFFFF;
    }*/
  } else {
    iLength = m_avFormat->duration;
  }
  return ConvertTimestampToRT(iLength, 1, AV_TIME_BASE);
}

// Pin creation
STDMETHODIMP CLAVFSplitter::CreateOutputs()
{
  CAutoLock lock(this);

  int ret; // return code from avformat functions

  // Convert the filename from wchar to char for avformat
  char fileName[1024];
  wcstombs_s(NULL, fileName, 1024, m_fileName.c_str(), _TRUNCATE);

  ret = av_open_input_file(&m_avFormat, fileName, NULL, FFMPEG_FILE_BUFFER_SIZE, NULL);
  if (ret < 0) {
    DbgLog((LOG_ERROR, 0, TEXT("av_open_input_file failed")));
    goto fail;
  }

  ret = av_find_stream_info(m_avFormat);
  if (ret < 0) {
    DbgLog((LOG_ERROR, 0, TEXT("av_find_stream_info failed")));
    goto fail;
  }

  m_bMatroska = (_stricmp(m_avFormat->iformat->name, "matroska") == 0);
  m_bAVI = (_stricmp(m_avFormat->iformat->name, "avi") == 0);

  // try to use non-blocking methods
  m_avFormat->flags |= AVFMT_FLAG_NONBLOCK;

  m_rtNewStart = m_rtStart = m_rtCurrent = 0;
  m_rtNewStop = m_rtStop = m_rtDuration = GetStreamLength();

  // TODO Programms support
  ASSERT(m_avFormat->nb_programs == 0 || m_avFormat->nb_programs == 1);

  for(int i = 0; i < countof(m_streams); i++) {
    m_streams[i].Clear();
  }

  const char* container = m_avFormat->iformat->name;
  for(unsigned int streamId = 0; streamId < m_avFormat->nb_streams; streamId++)
  {
    AVStream* pStream = m_avFormat->streams[streamId];
    stream s;
    s.pid = streamId;
    s.streamInfo = new CDSStreamInfo(pStream, container);

    // HACK: Change codec_id to TEXT for SSA to prevent some evil doings
    if (pStream->codec->codec_id == CODEC_ID_SSA) {
      pStream->codec->codec_id = CODEC_ID_TEXT;
    }

    switch(pStream->codec->codec_type)
    {
    case AVMEDIA_TYPE_VIDEO:
      m_streams[video].push_back(s);
      break;
    case AVMEDIA_TYPE_AUDIO:
      m_streams[audio].push_back(s);
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      m_streams[subpic].push_back(s);
      break;
    default:
      // unsupported stream
      delete s.streamInfo;
      break;
    }
  }

  HRESULT hr = S_OK;

  // Try to create pins
  for(int i = 0; i < countof(m_streams); i++) {
    std::vector<stream>::iterator it;
    for ( it = m_streams[i].begin(); it != m_streams[i].end(); it++ ) {
      const WCHAR* name = CStreamList::ToString(i);

      std::vector<CMediaType> mts;
      mts.push_back(it->streamInfo->mtype);

      CLAVFOutputPin* pPin = new CLAVFOutputPin(mts, name, this, this, &hr, (StreamType)i, container);
      if(SUCCEEDED(hr)) {
        pPin->SetStreamId(it->pid);
        m_pPins.push_back(pPin);
        break;
      } else {
        delete pPin;
      }
    }
  }

  return S_OK;
fail:
  // Cleanup
  if (m_avFormat) {
    av_close_input_file(m_avFormat);
    m_avFormat = NULL;
  }
  return E_FAIL;
}

STDMETHODIMP CLAVFSplitter::DeleteOutputs()
{
  CAutoLock lock(this);
  if(m_State != State_Stopped) return VFW_E_NOT_STOPPED;

  if(m_avFormat) {
    av_close_input_file(m_avFormat);
    m_avFormat = NULL;
  }

  CAutoLock pinLock(&m_csPins);
  // Release pins
  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    if(IPin* pPinTo = (*it)->GetConnected()) pPinTo->Disconnect();
    (*it)->Disconnect();
    delete (*it);
  }
  m_pPins.clear();

  return S_OK;
}

bool CLAVFSplitter::IsAnyPinDrying()
{
  // MPC changes thread priority here
  // TODO: Investigate if that is needed
  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    if(!(*it)->IsDiscontinuous() && (*it)->QueueCount() < MIN_PACKETS_IN_QUEUE) {
      return true;
    }
  }
  return false;
}

DWORD CLAVFSplitter::GetVideoStreamId()
{
  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    if((*it)->IsVideoPin()) {
      return (*it)->GetStreamId();
    }
  }
  return m_pPins[0]->GetStreamId();
}


// Worker Thread
DWORD CLAVFSplitter::ThreadProc()
{
  m_fFlushing = false;
  m_eEndFlush.Set();

  for(DWORD cmd = (DWORD)-1; ; cmd = GetRequest())
  {
    if(cmd == CMD_EXIT)
    {
      m_hThread = NULL;
      Reply(S_OK);
      return 0;
    }

    SetThreadPriority(m_hThread, THREAD_PRIORITY_NORMAL);

    m_rtStart = m_rtNewStart;
    m_rtStop = m_rtNewStop;

    DemuxSeek(m_rtStart);

    if(cmd != (DWORD)-1)
      Reply(S_OK);

    // Wait for the end of any flush
    m_eEndFlush.Wait();

    if(!m_fFlushing) {
      std::vector<CLAVFOutputPin *>::iterator it;
      for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
        if ((*it)->IsConnected()) {
          (*it)->DeliverNewSegment(m_rtStart, m_rtStop, m_dRate);
        }
      }
    }

    m_bDiscontinuitySent.clear();

    HRESULT hr = S_OK;
    while(SUCCEEDED(hr) && !CheckRequest(&cmd)) {
      hr = DemuxNextPacket();
    }

    // If we didnt exit by request, deliver end-of-stream
    if(!CheckRequest(&cmd)) {
      std::vector<CLAVFOutputPin *>::iterator it;
      for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
        (*it)->QueueEndOfStream();
      }
    }

  }
  ASSERT(0); // we should only exit via CMD_EXIT

  m_hThread = NULL;
  return 0;
}

// Converts the lavf pts timestamp to a DShow REFERENCE_TIME
// Based on DVDDemuxFFMPEG
REFERENCE_TIME CLAVFSplitter::ConvertTimestampToRT(int64_t pts, int num, int den)
{
  if (pts == (int64_t)AV_NOPTS_VALUE) {
    return Packet::INVALID_TIME;
  }

  // Let av_rescale do the work, its smart enough to not overflow
  REFERENCE_TIME timestamp = av_rescale(pts, (int64_t)num * DSHOW_TIME_BASE, den);

  if (m_avFormat->start_time != (int64_t)AV_NOPTS_VALUE && m_avFormat->start_time != 0) {
    timestamp -= av_rescale(m_avFormat->start_time, DSHOW_TIME_BASE, AV_TIME_BASE);
  }

  return timestamp;
}

// Converts the lavf pts timestamp to a DShow REFERENCE_TIME
// Based on DVDDemuxFFMPEG
int64_t CLAVFSplitter::ConvertRTToTimestamp(REFERENCE_TIME timestamp, int num, int den)
{
  if (timestamp == Packet::INVALID_TIME) {
    return (int64_t)AV_NOPTS_VALUE;
  }

  // Let av_rescale do the work, its smart enough to not overflow
  if (m_avFormat->start_time != (int64_t)AV_NOPTS_VALUE && m_avFormat->start_time != 0) {
    timestamp += av_rescale(m_avFormat->start_time, DSHOW_TIME_BASE, AV_TIME_BASE);
  }

  return av_rescale(timestamp, den, (int64_t)num * DSHOW_TIME_BASE);
}

// Seek to the specified time stamp
// Based on DVDDemuxFFMPEG
HRESULT CLAVFSplitter::DemuxSeek(REFERENCE_TIME rtStart)
{
  if(rtStart < 0) { rtStart = 0; }

  DWORD streamId = GetVideoStreamId();
  AVStream *stream = m_avFormat->streams[streamId];

  int64_t seek_pts = ConvertRTToTimestamp(rtStart, stream->time_base.num, stream->time_base.den);

  int ret = avformat_seek_file(m_avFormat, streamId, _I64_MIN, seek_pts, _I64_MAX, 0);
  //int ret = av_seek_frame(m_avFormat, -1, seek_pts, 0);

  return S_OK;
}

// Demux the next packet and deliver it to the output pins
// Based on DVDDemuxFFMPEG
HRESULT CLAVFSplitter::DemuxNextPacket()
{
  bool bReturnEmpty = false;

  // Read packet
  AVPacket pkt;
  Packet *pPacket = NULL;

  // assume we are not eof
  if(m_avFormat->pb) {
    m_avFormat->pb->eof_reached = 0;
  }

  int result = 0;
  try {
    result = av_read_frame(m_avFormat, &pkt);
  } catch(...) {
    // ignore..
  }

  if (result == AVERROR(EINTR) || result == AVERROR(EAGAIN))
  {
    // timeout, probably no real error, return empty packet
    bReturnEmpty = true;
  } else if (result < 0) {
    // meh, fail
  } else if (pkt.size < 0 || pkt.stream_index >= MAX_STREAMS) {
    // XXX, in some cases ffmpeg returns a negative packet size
    if(m_avFormat->pb && !m_avFormat->pb->eof_reached) {
      bReturnEmpty = true;
    }
    av_free_packet(&pkt);
  } else {
    AVStream *stream = m_avFormat->streams[pkt.stream_index];
    pPacket = new Packet();

    // libavformat sometimes bugs and sends dts/pts 0 instead of invalid.. correct this
    if(pkt.dts == 0) {
      pkt.dts = AV_NOPTS_VALUE;
    }
    if(pkt.pts == 0) {
      pkt.pts = AV_NOPTS_VALUE;
    }

    // we need to get duration slightly different for matroska embedded text subtitels
    if(m_bMatroska && stream->codec->codec_id == CODEC_ID_TEXT && pkt.convergence_duration != 0) {
      pkt.duration = (int)pkt.convergence_duration;
    }

    if(m_bAVI && stream->codec && stream->codec->codec_type == CODEC_TYPE_VIDEO)
    {
      // AVI's always have borked pts, specially if m_pFormatContext->flags includes
      // AVFMT_FLAG_GENPTS so always use dts
      pkt.pts = AV_NOPTS_VALUE;
    }

    if(pkt.data) {
      pPacket->SetData(pkt.data, pkt.size);
    }

    pPacket->StreamId = (DWORD)pkt.stream_index;

    REFERENCE_TIME pts = (REFERENCE_TIME)ConvertTimestampToRT(pkt.pts, stream->time_base.num, stream->time_base.den);
    REFERENCE_TIME dts = (REFERENCE_TIME)ConvertTimestampToRT(pkt.dts, stream->time_base.num, stream->time_base.den);
    REFERENCE_TIME duration = (REFERENCE_TIME)ConvertTimestampToRT(pkt.duration, stream->time_base.num, stream->time_base.den);

    REFERENCE_TIME rt = m_rtCurrent;
    // Try the different times set, pts first, dts when pts is not valid
    if (pts != Packet::INVALID_TIME) {
      rt = pts;
    } else if (dts != Packet::INVALID_TIME) {
      rt = dts;
    }

    // stupid VC1
    if (stream->codec->codec_id == CODEC_ID_VC1 && dts != Packet::INVALID_TIME) {
      rt = dts;
    }

    pPacket->rtStart = rt;
    pPacket->rtStop = rt + ((duration > 0) ? duration : 1);

    if (stream->codec->codec_type == CODEC_TYPE_SUBTITLE) {
      pPacket->bDiscontinuity = TRUE;
    } else {
      pPacket->bSyncPoint = (duration > 0) ? 1 : 0;
      pPacket->bAppendable = !pPacket->bSyncPoint;
    }

    av_free_packet(&pkt);
  }

  if (bReturnEmpty && !pPacket) {
    return S_FALSE;
  }
  if (!pPacket) {
    return E_FAIL;
  }

  return DeliverPacket(pPacket);
}

HRESULT CLAVFSplitter::DeliverPacket(Packet *pPacket)
{
  HRESULT hr = S_FALSE;

  CLAVFOutputPin* pPin = GetOutputPin(pPacket->StreamId);
  if(!pPin || !pPin->IsConnected()) {
    delete pPacket;
    return S_FALSE;
  }

  if(pPacket->rtStart != Packet::INVALID_TIME) {
    m_rtCurrent = pPacket->rtStart;

    pPacket->rtStart -= m_rtStart;
    pPacket->rtStop -= m_rtStart;

    ASSERT(pPacket->rtStart <= pPacket->rtStop);
  }

  if(m_bDiscontinuitySent.find(pPacket->StreamId) == m_bDiscontinuitySent.end()) {
    pPacket->bDiscontinuity = true;
  }

  BOOL bDiscontinuity = pPacket->bDiscontinuity; 

  hr = pPin->QueuePacket(pPacket);

  // TODO track active pins

  if(bDiscontinuity) {
    m_bDiscontinuitySent.insert(pPacket->StreamId);
  }

  return hr;
}

// State Control
STDMETHODIMP CLAVFSplitter::Stop()
{
  CAutoLock cAutoLock(this);

  DeliverBeginFlush();
  CallWorker(CMD_EXIT);
  DeliverEndFlush();

  HRESULT hr;
  if(FAILED(hr = __super::Stop())) {
    return hr;
  }

  return S_OK;
}

STDMETHODIMP CLAVFSplitter::Pause()
{
  CAutoLock cAutoLock(this);

  FILTER_STATE fs = m_State;

  HRESULT hr;
  if(FAILED(hr = __super::Pause())) {
    return hr;
  }

  // The filter graph will set us to pause before running
  // So if we were stopped before, create the thread
  // Note that the splitter will always be running,
  // and even in pause mode fill up the buffers
  if(fs == State_Stopped) {
    Create();
  }

  return S_OK;
}

STDMETHODIMP CLAVFSplitter::Run(REFERENCE_TIME tStart)
{
  CAutoLock cAutoLock(this);

  HRESULT hr;
  if(FAILED(hr = __super::Run(tStart))) {
    return hr;
  }

  return S_OK;
}

// Flushing
void CLAVFSplitter::DeliverBeginFlush()
{
  m_fFlushing = true;

  // flush all pins
  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    (*it)->DeliverBeginFlush();
  }
}

void CLAVFSplitter::DeliverEndFlush()
{
  // flush all pins
  std::vector<CLAVFOutputPin *>::iterator it;
  for(it = m_pPins.begin(); it != m_pPins.end(); it++) {
    (*it)->DeliverEndFlush();
  }

  m_fFlushing = false;
  m_eEndFlush.Set();
}

// IMediaSeeking
STDMETHODIMP CLAVFSplitter::GetCapabilities(DWORD* pCapabilities)
{
  CheckPointer(pCapabilities, E_POINTER);

  *pCapabilities =
    AM_SEEKING_CanGetStopPos   |
    AM_SEEKING_CanGetDuration  |
    AM_SEEKING_CanSeekAbsolute |
    AM_SEEKING_CanSeekForwards |
    AM_SEEKING_CanSeekBackwards;

  return S_OK;
}

STDMETHODIMP CLAVFSplitter::CheckCapabilities(DWORD* pCapabilities)
{
  CheckPointer(pCapabilities, E_POINTER);
  // capabilities is empty, all is good
  if(*pCapabilities == 0) return S_OK;
  // read caps
  DWORD caps;
  GetCapabilities(&caps);

  // Store the caps that we wanted
  DWORD wantCaps = *pCapabilities;
  // Update pCapabilities with what we have
  *pCapabilities = caps & wantCaps;

  // if nothing matches, its a disaster!
  if(*pCapabilities == 0) return E_FAIL;
  // if all matches, its all good
  if(*pCapabilities == wantCaps) return S_OK;
  // otherwise, a partial match
  return S_FALSE;
}

STDMETHODIMP CLAVFSplitter::IsFormatSupported(const GUID* pFormat) {return !pFormat ? E_POINTER : *pFormat == TIME_FORMAT_MEDIA_TIME ? S_OK : S_FALSE;}
STDMETHODIMP CLAVFSplitter::QueryPreferredFormat(GUID* pFormat) {return GetTimeFormat(pFormat);}
STDMETHODIMP CLAVFSplitter::GetTimeFormat(GUID* pFormat) {return pFormat ? *pFormat = TIME_FORMAT_MEDIA_TIME, S_OK : E_POINTER;}
STDMETHODIMP CLAVFSplitter::IsUsingTimeFormat(const GUID* pFormat) {return IsFormatSupported(pFormat);}
STDMETHODIMP CLAVFSplitter::SetTimeFormat(const GUID* pFormat) {return S_OK == IsFormatSupported(pFormat) ? S_OK : E_INVALIDARG;}
STDMETHODIMP CLAVFSplitter::GetDuration(LONGLONG* pDuration) {CheckPointer(pDuration, E_POINTER); *pDuration = m_rtDuration; return S_OK;}
STDMETHODIMP CLAVFSplitter::GetStopPosition(LONGLONG* pStop) {return GetDuration(pStop);}
STDMETHODIMP CLAVFSplitter::GetCurrentPosition(LONGLONG* pCurrent) {return E_NOTIMPL;}
STDMETHODIMP CLAVFSplitter::ConvertTimeFormat(LONGLONG* pTarget, const GUID* pTargetFormat, LONGLONG Source, const GUID* pSourceFormat) {return E_NOTIMPL;}
STDMETHODIMP CLAVFSplitter::SetPositions(LONGLONG* pCurrent, DWORD dwCurrentFlags, LONGLONG* pStop, DWORD dwStopFlags)
{
  CAutoLock cAutoLock(this);

  if(!pCurrent && !pStop
    || (dwCurrentFlags&AM_SEEKING_PositioningBitsMask) == AM_SEEKING_NoPositioning 
    && (dwStopFlags&AM_SEEKING_PositioningBitsMask) == AM_SEEKING_NoPositioning) {
      return S_OK;
  }

  REFERENCE_TIME
    rtCurrent = m_rtCurrent,
    rtStop = m_rtStop;

  if(pCurrent) {
    switch(dwCurrentFlags&AM_SEEKING_PositioningBitsMask)
    {
    case AM_SEEKING_NoPositioning: break;
    case AM_SEEKING_AbsolutePositioning: rtCurrent = *pCurrent; break;
    case AM_SEEKING_RelativePositioning: rtCurrent = rtCurrent + *pCurrent; break;
    case AM_SEEKING_IncrementalPositioning: rtCurrent = rtCurrent + *pCurrent; break;
    }
  }

  if(pStop){
    switch(dwStopFlags&AM_SEEKING_PositioningBitsMask)
    {
    case AM_SEEKING_NoPositioning: break;
    case AM_SEEKING_AbsolutePositioning: rtStop = *pStop; break;
    case AM_SEEKING_RelativePositioning: rtStop += *pStop; break;
    case AM_SEEKING_IncrementalPositioning: rtStop = rtCurrent + *pStop; break;
    }
  }

  if(m_rtCurrent == rtCurrent && m_rtStop == rtStop) {
    return S_OK;
  }

  m_rtNewStart = m_rtCurrent = rtCurrent;
  m_rtNewStop = rtStop;

  if(ThreadExists())
  {
    DeliverBeginFlush();
    CallWorker(CMD_SEEK);
    DeliverEndFlush();
  }

  return S_OK;
}
STDMETHODIMP CLAVFSplitter::GetPositions(LONGLONG* pCurrent, LONGLONG* pStop)
{
  if(pCurrent) *pCurrent = m_rtCurrent;
  if(pStop) *pStop = m_rtStop;
  return S_OK;
}
STDMETHODIMP CLAVFSplitter::GetAvailable(LONGLONG* pEarliest, LONGLONG* pLatest)
{
  if(pEarliest) *pEarliest = 0;
  return GetDuration(pLatest);
}
STDMETHODIMP CLAVFSplitter::SetRate(double dRate) {return dRate > 0 ? m_dRate = dRate, S_OK : E_INVALIDARG;}
STDMETHODIMP CLAVFSplitter::GetRate(double* pdRate) {return pdRate ? *pdRate = m_dRate, S_OK : E_POINTER;}
STDMETHODIMP CLAVFSplitter::GetPreroll(LONGLONG* pllPreroll) {return pllPreroll ? *pllPreroll = 0, S_OK : E_POINTER;}

// IAMExtendedSeeking
STDMETHODIMP CLAVFSplitter::get_ExSeekCapabilities(long* pExCapabilities)
{
  CheckPointer(pExCapabilities, E_POINTER);
  *pExCapabilities = AM_EXSEEK_CANSEEK;
  if(m_avFormat->nb_chapters > 0) *pExCapabilities |= AM_EXSEEK_MARKERSEEK;
  return S_OK;
}

STDMETHODIMP CLAVFSplitter::get_MarkerCount(long* pMarkerCount)
{
  CheckPointer(pMarkerCount, E_POINTER);
  *pMarkerCount = (long)m_avFormat->nb_chapters;
  return S_OK;
}

STDMETHODIMP CLAVFSplitter::get_CurrentMarker(long* pCurrentMarker)
{
  CheckPointer(pCurrentMarker, E_POINTER);
  // Can the time_base change in between chapters?
  // Anyhow, we do the calculation in the loop, just to be safe
  for(unsigned int i = 0; i < m_avFormat->nb_chapters; i++) {
    int64_t pts = ConvertRTToTimestamp(m_rtCurrent, m_avFormat->chapters[i]->time_base.den, m_avFormat->chapters[i]->time_base.num);
    if (pts >= m_avFormat->chapters[i]->start && pts <= m_avFormat->chapters[i]->end) {
      *pCurrentMarker = (i + 1);
      return S_OK;
    }
  }
  return E_FAIL;
}

STDMETHODIMP CLAVFSplitter::GetMarkerTime(long MarkerNum, double* pMarkerTime)
{
  CheckPointer(pMarkerTime, E_POINTER);
  // Chapters go by a 1-based index, doh
  unsigned int index = MarkerNum - 1;
  if(index >= m_avFormat->nb_chapters) { return E_FAIL; }

  REFERENCE_TIME rt = ConvertTimestampToRT(m_avFormat->chapters[index]->start, m_avFormat->chapters[index]->time_base.num, m_avFormat->chapters[index]->time_base.den);
  *pMarkerTime = (double)rt / DSHOW_TIME_BASE;

  return S_OK;
}

STDMETHODIMP CLAVFSplitter::GetMarkerName(long MarkerNum, BSTR* pbstrMarkerName)
{
  CheckPointer(pbstrMarkerName, E_POINTER);
  // Chapters go by a 1-based index, doh
  unsigned int index = MarkerNum - 1;
  if(index >= m_avFormat->nb_chapters) { return E_FAIL; }
  // Get the title, or generate one
  OLECHAR wTitle[128];
  if (av_metadata_get(m_avFormat->chapters[index]->metadata, "title", NULL, 0)) {
    char *title = av_metadata_get(m_avFormat->chapters[index]->metadata, "title", NULL, 0)->value;
    mbstowcs_s(NULL, wTitle, title, _TRUNCATE);
  } else {
    swprintf_s(wTitle, L"Chapter %d", MarkerNum);
  }
  *pbstrMarkerName = SysAllocString(wTitle);
  return S_OK;
}

// IKeyFrameInfo
STDMETHODIMP CLAVFSplitter::GetKeyFrameCount(UINT& nKFs)
{
  AVStream *stream = m_avFormat->streams[GetVideoStreamId()];
  nKFs = stream->nb_index_entries;
  return (stream->nb_index_entries == stream->nb_frames) ? S_FALSE : S_OK;
}

STDMETHODIMP CLAVFSplitter::GetKeyFrames(const GUID* pFormat, REFERENCE_TIME* pKFs, UINT& nKFs)
{
  CheckPointer(pFormat, E_POINTER);
  CheckPointer(pKFs, E_POINTER);

  if(*pFormat != TIME_FORMAT_MEDIA_TIME) return E_INVALIDARG;

  AVStream *stream = m_avFormat->streams[GetVideoStreamId()];
  nKFs = stream->nb_index_entries;
  for(unsigned int i = 0; i < nKFs; i++) {
    pKFs[i] = ConvertTimestampToRT(stream->index_entries[i].timestamp, stream->time_base.num, stream->time_base.den);
  }
  return S_OK;
}

STDMETHODIMP CLAVFSplitter::RenameOutputPin(DWORD TrackNumSrc, DWORD TrackNumDst, const AM_MEDIA_TYPE* pmt)
{
  CLAVFOutputPin* pPin = GetOutputPin(TrackNumSrc);
  // Output Pin was found
  // Stop the Graph, remove the old filter, render the graph again, start it up again
  // -- only for audio --
  // other pins we can't dynamically reconnect, its not supported for video on most codecs/renderes
  // and subtitles are done by those as well, most of the time
  if (pPin && pPin->IsAudioPin()) {
    CAutoLock lock(this);
    HRESULT hr = S_OK;

    IMediaControl *pControl = NULL;
    hr = m_pGraph->QueryInterface(IID_IMediaControl, (void **)&pControl);

    // Stop the filter graph
    pControl->Stop();
    // Update Output Pin
    pPin->SetStreamId(TrackNumDst);
    pPin->SetNewMediaType(*pmt);

    // If the pin was connected, disconnect it
    if (pPin->IsConnected()) {
      // Get the Info about the old transform filter
      PIN_INFO pInfo;
      pPin->GetConnected()->QueryPinInfo(&pInfo);

      // Remove old transform filter
      m_pGraph->RemoveFilter(pInfo.pFilter);
    }

    // Use IGraphBuilder to rebuild the graph
    IGraphBuilder *pGraphBuilder = NULL;
    if(SUCCEEDED(hr = m_pGraph->QueryInterface(__uuidof(IGraphBuilder), (void **)&pGraphBuilder))) {
      // Instruct the GraphBuilder to connect us again
      hr = pGraphBuilder->Render(pPin);
      pGraphBuilder->Release();
    }
    if (SUCCEEDED(hr)) {
      // Re-start the graph
      hr = pControl->Run();
    }
    pControl->Release();

    return hr;
  } else if (pPin) {
    CAutoLock pinLock(&m_csPins);
    // Old fashioned reconnect
    if(IPin *pinTo = pPin->GetConnected()) {
      if(pmt && FAILED(pinTo->QueryAccept(pmt))) {
        return VFW_E_TYPE_NOT_ACCEPTED;
      }
      pPin->SetStreamId(TrackNumDst);
      if(pmt) {
        pPin->QueueMediaType(*pmt);
      }
      return S_OK;
    }
  }
  return E_FAIL;
}

// IAMStreamSelect
STDMETHODIMP CLAVFSplitter::Count(DWORD *pcStreams)
{
  CheckPointer(pcStreams, E_POINTER);

  *pcStreams = 0;
  for(int i = 0; i < countof(m_streams); i++) {
    *pcStreams += (DWORD)m_streams[i].size();
  }

  return S_OK;
}

STDMETHODIMP CLAVFSplitter::Enable(long lIndex, DWORD dwFlags)
{
  if(!(dwFlags & AMSTREAMSELECTENABLE_ENABLE)) {
    return E_NOTIMPL;
  }

  for(int i = 0, j = 0; i < countof(m_streams); i++) {
    int cnt = (int)m_streams[i].size();

    if(lIndex >= j && lIndex < j+cnt) {
      long idx = (lIndex - j);

      stream& to = m_streams[i].at(idx);

      std::vector<stream>::iterator it;
      for(it = m_streams[i].begin(); it != m_streams[i].end(); it++) {
        if(!GetOutputPin(it->pid)) {
          continue;
        }

        HRESULT hr;
        if(FAILED(hr = RenameOutputPin(*it, to, &to.streamInfo->mtype))) {
          return hr;
        }
        return S_OK;
      }
    }
    j += cnt;
  }
  return S_FALSE;
}

#define INFOBUFSIZE 128
STDMETHODIMP CLAVFSplitter::Info(long lIndex, AM_MEDIA_TYPE **ppmt, DWORD *pdwFlags, LCID *plcid, DWORD *pdwGroup, WCHAR **ppszName, IUnknown **ppObject, IUnknown **ppUnk)
{
  for(int i = 0, j = 0; i < countof(m_streams); i++) {
    int cnt = (int)m_streams[i].size();

    if(lIndex >= j && lIndex < j+cnt) {
      long idx = (lIndex - j);

      stream& s = m_streams[i].at(idx);

      if(ppmt) *ppmt = CreateMediaType(&s.streamInfo->mtype);
      if(pdwFlags) *pdwFlags = GetOutputPin(s) ? (AMSTREAMSELECTINFO_ENABLED|AMSTREAMSELECTINFO_EXCLUSIVE) : 0;
      if(pdwGroup) *pdwGroup = i;
      if(ppObject) *ppObject = NULL;
      if(ppUnk) *ppUnk = NULL;

      // Probe language
      if (plcid) {
        if (av_metadata_get(m_avFormat->streams[s.pid]->metadata, "language", NULL, 0)) {
          char *lang = av_metadata_get(m_avFormat->streams[s.pid]->metadata, "language", NULL, 0)->value;
          *plcid = ProbeLangForLCID(lang);
        }
      }

      // Populate stream name
      if(ppszName) {
        lavf_describe_stream(m_avFormat->streams[s.pid], ppszName);
      }
    }
    j += cnt;
  }

  return S_OK;
}

// CStreamList
const WCHAR* CLAVFSplitter::CStreamList::ToString(int type)
{
  return 
    type == video ? L"Video" :
    type == audio ? L"Audio" :
    type == subpic ? L"Subtitle" :
    L"Unknown";
}

const CLAVFSplitter::stream* CLAVFSplitter::CStreamList::FindStream(DWORD pid)
{
  std::vector<stream>::iterator it;
  for ( it = begin(); it != end(); it++ ) {
    if ((*it).pid == pid) {
      return &(*it);
    }
  }

  return NULL;
}

void CLAVFSplitter::CStreamList::Clear()
{
  std::vector<stream>::iterator it;
  for ( it = begin(); it != end(); it++ ) {
    delete (*it).streamInfo;
  }
  __super::clear();
}
