/* -----------------------------------------------------------------------------
Software Copyright License for the Fraunhofer Software Library VVenc

(c) Copyright (2019-2020) Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V. 

1.    INTRODUCTION

The Fraunhofer Software Library VVenc (“Fraunhofer Versatile Video Encoding Library”) is software that implements (parts of) the Versatile Video Coding Standard - ITU-T H.266 | MPEG-I - Part 3 (ISO/IEC 23090-3) and related technology. 
The standard contains Fraunhofer patents as well as third-party patents. Patent licenses from third party standard patent right holders may be required for using the Fraunhofer Versatile Video Encoding Library. It is in your responsibility to obtain those if necessary. 

The Fraunhofer Versatile Video Encoding Library which mean any source code provided by Fraunhofer are made available under this software copyright license. 
It is based on the official ITU/ISO/IEC VVC Test Model (VTM) reference software whose copyright holders are indicated in the copyright notices of its source files. The VVC Test Model (VTM) reference software is licensed under the 3-Clause BSD License and therefore not subject of this software copyright license.

2.    COPYRIGHT LICENSE

Internal use of the Fraunhofer Versatile Video Encoding Library, in source and binary forms, with or without modification, is permitted without payment of copyright license fees for non-commercial purposes of evaluation, testing and academic research. 

No right or license, express or implied, is granted to any part of the Fraunhofer Versatile Video Encoding Library except and solely to the extent as expressly set forth herein. Any commercial use or exploitation of the Fraunhofer Versatile Video Encoding Library and/or any modifications thereto under this license are prohibited.

For any other use of the Fraunhofer Versatile Video Encoding Library than permitted by this software copyright license You need another license from Fraunhofer. In such case please contact Fraunhofer under the CONTACT INFORMATION below.

3.    LIMITED PATENT LICENSE

As mentioned under 1. Fraunhofer patents are implemented by the Fraunhofer Versatile Video Encoding Library. If You use the Fraunhofer Versatile Video Encoding Library in Germany, the use of those Fraunhofer patents for purposes of testing, evaluating and research and development is permitted within the statutory limitations of German patent law. However, if You use the Fraunhofer Versatile Video Encoding Library in a country where the use for research and development purposes is not permitted without a license, you must obtain an appropriate license from Fraunhofer. It is Your responsibility to check the legal requirements for any use of applicable patents.    

Fraunhofer provides no warranty of patent non-infringement with respect to the Fraunhofer Versatile Video Encoding Library.


4.    DISCLAIMER

The Fraunhofer Versatile Video Encoding Library is provided by Fraunhofer "AS IS" and WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, including but not limited to the implied warranties fitness for a particular purpose. IN NO EVENT SHALL FRAUNHOFER BE LIABLE for any direct, indirect, incidental, special, exemplary, or consequential damages, including but not limited to procurement of substitute goods or services; loss of use, data, or profits, or business interruption, however caused and on any theory of liability, whether in contract, strict liability, or tort (including negligence), arising in any way out of the use of the Fraunhofer Versatile Video Encoding Library, even if advised of the possibility of such damage.

5.    CONTACT INFORMATION

Fraunhofer Heinrich Hertz Institute
Attention: Video Coding & Analytics Department
Einsteinufer 37
10587 Berlin, Germany
www.hhi.fraunhofer.de/vvc
vvc@hhi.fraunhofer.de
----------------------------------------------------------------------------- */


/** \file     EncLib.cpp
    \brief    encoder class
*/

#include "EncLib.h"

#include "../../../include/vvenc/EncoderIf.h"
#include "CommonLib/Picture.h"
#include "CommonLib/CommonDef.h"
#include "CommonLib/TimeProfiler.h"
#include "CommonLib/Rom.h"
#include "Utilities/NoMallocThreadPool.h"
#include "vvenc/EncoderIf.h"

//! \ingroup EncoderLib
//! \{

namespace vvenc {

// ====================================================================================================================
// Constructor / destructor / create / destroy
// ====================================================================================================================

EncLib::EncLib()
  : m_numPicsRcvd   ( 0 )
  , m_numPicsInQueue( 0 )
  , m_numPicsCoded  ( 0 )
  , m_pocEncode     ( -1 )
  , m_pocRecOut     ( 0 )
  , m_yuvWriterIf   ( nullptr )
 , m_threadPool     ( nullptr )
  , m_spsMap        ( MAX_NUM_SPS )
  , m_ppsMap        ( MAX_NUM_PPS )
  , m_GOPSizeLog2   ( -1 )
  , m_TicksPerFrameMul4 ( 0 )
{
}

EncLib::~EncLib()
{
}

void EncLib::init( const EncCfg& encCfg, YUVWriterIf* yuvWriterIf )
{
  // copy config parameter
  const_cast<EncCfg&>(m_cEncCfg).setCfgParameter( encCfg );

  m_yuvWriterIf = yuvWriterIf;

#if ENABLE_TRACING
  g_trace_ctx = tracing_init( m_cEncCfg.m_traceFile, m_cEncCfg.m_traceRule );
  if( g_trace_ctx && m_cEncCfg.m_listTracingChannels )
  {
    std::string sChannelsList;
    g_trace_ctx->getChannelsList( sChannelsList );
    msg( INFO, "\n Using tracing channels:\n\n%s\n", sChannelsList.c_str() );
  }
#endif

  // setup parameter sets
  const int dciId = m_cEncCfg.m_decodingParameterSetEnabled ? 1 : 0;
  SPS& sps0       = *( m_spsMap.allocatePS(0) ); // NOTE: implementations that use more than 1 SPS need to be aware of activation issues.
  PPS& pps0       = *( m_ppsMap.allocatePS(0) );

  xInitSPS( sps0 );
  sps0.dciId = m_cDCI.dciId;
  xInitVPS( m_cVPS );
  xInitDCI( m_cDCI, sps0, dciId );
  xInitPPS( pps0, sps0 );
  xInitRPL( sps0 );

  if ( encCfg.m_numWppThreads > 0 )
  {
    const int maxCntEnc = ( encCfg.m_numWppThreads > 0 ) ? std::min( (int)pps0.pcv->heightInCtus, encCfg.m_numWppThreads) : 1;
    m_threadPool = new NoMallocThreadPool( maxCntEnc, "EncSliceThreadPool" );
  }

  m_MCTF.init( m_cEncCfg.m_internalBitDepth, m_cEncCfg.m_SourceWidth, m_cEncCfg.m_SourceHeight, sps0.CTUSize,
               m_cEncCfg.m_internChromaFormat, m_cEncCfg.m_QP, m_cEncCfg.m_MCTFFrames, m_cEncCfg.m_MCTFStrengths,
               m_cEncCfg.m_MCTFFutureReference, m_cEncCfg.m_MCTF,
               m_cEncCfg.m_MCTFNumLeadFrames, m_cEncCfg.m_MCTFNumTrailFrames, m_cEncCfg.m_framesToBeEncoded, m_threadPool );

  m_cGOPEncoder.init( m_cEncCfg, sps0, pps0, m_cRateCtrl, m_threadPool );

  m_pocToGopId.resize( m_cEncCfg.m_GOPSize, -1 );
  m_nextPocOffset.resize( m_cEncCfg.m_GOPSize, 0 );
  for ( int i = 0; i < m_cEncCfg.m_GOPSize; i++ )
  {
    const int poc = m_cEncCfg.m_GOPList[ i ].m_POC % m_cEncCfg.m_GOPSize;
    CHECK( m_cEncCfg.m_GOPList[ i ].m_POC > m_cEncCfg.m_GOPSize, "error: poc greater than gop size" );
    CHECK( m_pocToGopId[ poc ] != -1, "error: multiple entries in gop list map to same poc modulo gop size" );
    m_pocToGopId[ poc ] = i;
    const int nextGopNum = ( i + 1 ) / m_cEncCfg.m_GOPSize;
    const int nextGopId  = ( i + 1 ) % m_cEncCfg.m_GOPSize;
    const int nextPoc    = nextGopNum * m_cEncCfg.m_GOPSize + m_cEncCfg.m_GOPList[ nextGopId ].m_POC;
    m_nextPocOffset[ poc ] = nextPoc - m_cEncCfg.m_GOPList[ i ].m_POC;
  }
  for ( int i = 0; i < m_cEncCfg.m_GOPSize; i++ )
  {
    CHECK( m_pocToGopId   [ i ] < 0 || m_nextPocOffset[ i ] == 0, "error: poc not found in gop list" );
  }

  if ( encCfg.m_RCRateControlMode )
  {
    m_cRateCtrl.init( encCfg.m_RCRateControlMode, encCfg.m_framesToBeEncoded, encCfg.m_RCTargetBitrate, (int)( (double)encCfg.m_FrameRate / encCfg.m_temporalSubsampleRatio + 0.5 ), encCfg.m_IntraPeriod, encCfg.m_GOPSize, encCfg.m_SourceWidth, encCfg.m_SourceHeight,
      encCfg.m_CTUSize, encCfg.m_CTUSize, encCfg.m_internalBitDepth[ CH_L ], encCfg.m_RCKeepHierarchicalBit, encCfg.m_RCUseLCUSeparateModel, encCfg.m_GOPList );
  }

  int iOffset = -1;
  while((1<<(++iOffset)) < m_cEncCfg.m_GOPSize);
  m_GOPSizeLog2 = iOffset;

  if( m_cEncCfg.m_FrameRate )
  {
    int iTempRate = m_cEncCfg.m_FrameRate;
    int iTempScale = 1;
    switch( m_cEncCfg.m_FrameRate )
    {
    case 23: iTempRate = 24000; iTempScale = 1001; break;
    case 29: iTempRate = 30000; iTempScale = 1001; break;
    case 59: iTempRate = 60000; iTempScale = 1001; break;
    default: break;
    }
    m_TicksPerFrameMul4 = (int)((int64_t)4 *(int64_t)m_cEncCfg.m_TicksPerSecond * (int64_t)iTempScale/(int64_t)iTempRate);
  }

#if ENABLE_TIME_PROFILING
  if( g_timeProfiler == nullptr )
  {
    g_timeProfiler = new TimeProfiler();
  }
#elif ENABLE_TIME_PROFILING_EXTENDED
  if( g_timeProfiler == nullptr )
  {
#if ENABLE_TIME_PROFILING_PIC_TYPES
    g_timeProfiler = new TimeProfiler2D( 3, 1 );
#elif ENABLE_TIME_PROFILING_CTUS_IN_PIC
    int   widthInCTU  = ( m_cEncCfg.m_SourceWidth % m_cEncCfg.m_CTUSize )  ? m_cEncCfg.m_SourceWidth/m_cEncCfg.m_CTUSize  + 1 : m_cEncCfg.m_SourceWidth/m_cEncCfg.m_CTUSize;
    int   heightInCTU = ( m_cEncCfg.m_SourceHeight % m_cEncCfg.m_CTUSize ) ? m_cEncCfg.m_SourceHeight/m_cEncCfg.m_CTUSize + 1 : m_cEncCfg.m_SourceHeight/m_cEncCfg.m_CTUSize;
    g_timeProfiler = new TimeProfiler2D( widthInCTU, heightInCTU, 2 );
#elif ENABLE_TIME_PROFILING_CU_SHAPES
    g_timeProfiler = new TimeProfiler2D( Log2(m_cEncCfg.m_CTUSize) + 1, Log2(m_cEncCfg.m_CTUSize) + 1, 2 );
#endif
  }
#endif
}

void EncLib::destroy()
{
  m_MCTF.uninit();
  m_cRateCtrl.destroy();

#if ENABLE_CU_MODE_COUNTERS
  std::cout << std::endl;
  std::cout << "CU Modes statistic across picture types and temporal levels (0:Intra, >0:Inter, luma only)" << std::endl;
  for( size_t j = 0; j < g_cuCounters1D.getNumCntTypes(); j++ )
  {
    for( size_t i = 0; i < g_cuCounters1D.getDimHor() - 1; i++ )
    {
      g_cuCounters1D[j][0][g_cuCounters1D.getDimHor() - 1] += g_cuCounters1D[j][0][i];
    }
  }
  StatCounters::report2D( std::cout, g_cuCounters1D, false, true, false, true, true, CU_MODES_TESTED );

  std::cout << std::endl;
  std::cout << "CU Modes statistic across block-shapes (Non-I-Slices, luma only)" << std::endl;
  StatCounters::report2D( std::cout, g_cuCounters2D, true, true, false, true, true, CU_MODES_TESTED );
#endif

#if ENABLE_TIME_PROFILING
  if( g_timeProfiler )
  {
    std::cout << *g_timeProfiler;
    delete g_timeProfiler;
    g_timeProfiler = nullptr;
  }
#elif ENABLE_TIME_PROFILING_EXTENDED
  if( g_timeProfiler )
  {
#if ENABLE_TIME_PROFILING_PIC_TYPES
    std::cout << std::endl;
    std::cout << "Run-time of selected encoder stages across picture types (0:Intra, 1:Inter)" << std::endl;
    for( int j = 0; j < g_timeProfiler->getCountersSet()[0].getNumCntTypes(); j++ )
    {
      g_timeProfiler->getCountersSet()[0][j][0][2] += g_timeProfiler->getCountersSet()[0][j][0][0] + g_timeProfiler->getCountersSet()[0][j][0][1];
    }
    StatCounters::report2D( std::cout, g_timeProfiler->getCountersSet()[0], false, true, false, true, true, -1 );
#endif

#if ENABLE_TIME_PROFILING_CTUS_IN_PIC
    for( int i = 0; i < g_timeProfiler->getCountersSet().size(); i++ )
    {
      std::cout << "Run-time of selected encoder stages across CTUs of all pictures " << "(" << ( i == 0 ? "Intra": "Inter" << ")" ) << std::endl;
      StatCounters::report2D( std::cout, g_timeProfiler->getCountersSet()[i], false, true, false, true, true, -1 );
      if( i > 0 )
        g_timeProfiler->getCountersSet()[0] += g_timeProfiler->getCountersSet()[i];
    }
    if( g_timeProfiler->getCountersSet().size() > 1 )
    {
      std::cout << "Run-time of selected encoder stages across CTUs of all pictures (total)" << std::endl;
      StatCounters::report2D( std::cout, g_timeProfiler->getCountersSet()[0], false, true, false, true, true, -1 );
    }
#endif

#if ENABLE_TIME_PROFILING_CU_SHAPES
    for( int i = 0; i < g_timeProfiler->getCountersSet().size(); i++ )
    {
      std::cout << "Run-time of selected encoder stages across CU block shapes of all pictures " << "(" << ( i == 0 ? "Intra": "Inter" ) << ")"  << std::endl;
      StatCounters::report2D( std::cout, g_timeProfiler->getCountersSet()[i],  true, true, false, true, true, -1 );
      if( i > 0 ) g_timeProfiler->getCountersSet()[0] += g_timeProfiler->getCountersSet()[i];
    }
    if( g_timeProfiler->getCountersSet().size() > 1 )
    {
      std::cout << "Run-time of selected encoder stages across CU block shapes of all pictures (total)" << std::endl;
      StatCounters::report2D( std::cout, g_timeProfiler->getCountersSet()[0],  true, true, false, true, true, -1 );
    }
#endif
    delete g_timeProfiler;
    g_timeProfiler = nullptr;
  }
#endif

  if ( m_threadPool )
  {
    m_threadPool->shutdown( true );
    delete m_threadPool;
    m_threadPool = nullptr;
  }

  // internal picture buffer
  xDeletePicBuffer();

#if ENABLE_TRACING
  if ( g_trace_ctx )
  {
    tracing_uninit( g_trace_ctx );
  }
#endif
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================

void EncLib::encodePicture( bool flush, const YUVBuffer& yuvInBuf, AccessUnit& au, bool& isQueueEmpty )
{
  PROFILER_ACCUM_AND_START_NEW_SET( 1, g_timeProfiler, P_PIC_LEVEL );

  // clear output access unit
  au.m_bCtsValid = false;
  au.m_bDtsValid = false;
  au.m_bRAP      = false;
  au.m_cInfo     = "";
  au.m_iStatus   = 0;

  // setup picture and store original yuv
  Picture* pic = nullptr;
  if ( ! flush )
  {
    CHECK( m_ppsMap.getFirstPS() == nullptr || m_spsMap.getPS( m_ppsMap.getFirstPS()->spsId ) == nullptr, "picture set not initialised" );

    if ( m_cEncCfg.m_MCTF && m_numPicsRcvd <= 0 && m_MCTF.getNumLeadFrames() < m_cEncCfg.m_MCTFNumLeadFrames )
    {
      m_MCTF.addLeadFrame( yuvInBuf );
    }
    else if ( m_cEncCfg.m_MCTF && m_cEncCfg.m_framesToBeEncoded > 0 && m_numPicsRcvd >= m_cEncCfg.m_framesToBeEncoded )
    {
      m_MCTF.addTrailFrame( yuvInBuf );
    }
    else
    {
      PPS& pps = *(m_ppsMap.getFirstPS());
      const SPS& sps = *(m_spsMap.getPS( pps.spsId ));

      pic = xGetNewPicBuffer( pps, sps );

      PelUnitBuf yuvOrgBuf;
      setupPelUnitBuf( yuvInBuf, yuvOrgBuf, m_cEncCfg.m_internChromaFormat );

      pic->getOrigBuf().copyFrom( yuvOrgBuf );
      if( yuvInBuf.ctsValid )
      {
        pic->cts = yuvInBuf.cts;
        pic->ctsValid = true;
      }

      xInitPicture( *pic, m_numPicsRcvd, pps, sps, m_cVPS, m_cDCI );

      m_numPicsRcvd    += 1;
      m_numPicsInQueue += 1;
    }
  }

  // mctf filter
  int mctfDealy = 0;
  if ( m_cEncCfg.m_MCTF )
  {
    m_MCTF.filter( pic );
    mctfDealy = m_MCTF.getCurDelay();
  }

  // encode picture
  if ( m_numPicsInQueue >= m_cEncCfg.m_InputQueueSize
      || ( m_numPicsInQueue - mctfDealy > 0 && flush ) )
  {
    if ( m_cEncCfg.m_RCRateControlMode )
    {
      if ( m_numPicsRcvd == m_numPicsInQueue )
      {
        m_cRateCtrl.initRCGOP( 1 );
      }
      else if ( 1 == ( m_numPicsRcvd - m_numPicsInQueue ) % m_cEncCfg.m_GOPSize )
      {
        m_cRateCtrl.destroyRCGOP();
        m_cRateCtrl.initRCGOP( m_numPicsInQueue >= m_cEncCfg.m_GOPSize ? m_cEncCfg.m_GOPSize : m_numPicsInQueue );
      }
    }
    // update current poc
    m_pocEncode = ( m_pocEncode < 0 ) ? 0 : xGetNextPocICO( m_pocEncode, flush, m_numPicsRcvd );
    std::vector<Picture*> encList;
    xCreateCodingOrder( m_pocEncode, m_numPicsRcvd, m_numPicsInQueue, flush, encList );

    // create cts / dts
    if( !encList.empty() && encList[0]->ctsValid )
    {
      int64_t iDiffFrames = 0;
      int iNext = 0;
      if( !encList.empty() )
      {
        iNext = encList[0]->poc;
        iDiffFrames = ( m_numPicsCoded - iNext );
      }

      au.m_uiCts     = encList[0]->cts;
      au.m_bCtsValid = encList[0]->ctsValid;

      au.m_uiDts = ((iDiffFrames - m_GOPSizeLog2) * m_TicksPerFrameMul4)/4 + au.m_uiCts;
      au.m_bDtsValid = true;
      //assert(  (int64_t)au.m_uiDts < 0 || au.m_uiCts >= au.m_uiDts );
    }
    else
    {
      assert(0);
    }

    // encode picture with current poc
    m_cGOPEncoder.encodePicture( encList, m_cListPic, au, false );
    m_numPicsInQueue -= 1;
    m_numPicsCoded   += 1;
    // output reconstructed yuv
    xOutputRecYuv();
  }

  isQueueEmpty = ( m_numPicsInQueue <= 0 );

  if ( m_cEncCfg.m_RCRateControlMode && isQueueEmpty )
  {
    m_cRateCtrl.destroyRCGOP();
  }
}

void  EncLib::printSummary()
{
  m_cGOPEncoder.printOutSummary( m_numPicsCoded, m_cEncCfg.m_printMSEBasedSequencePSNR, m_cEncCfg.m_printSequenceMSE, m_cEncCfg.m_printHexPsnr, m_spsMap.getFirstPS()->bitDepths );
}

// ====================================================================================================================
// Protected member functions
// ====================================================================================================================


int EncLib::xGetNextPocICO( int poc, bool flush, int max ) const
{
  int chk  = 0;
  int next = ( poc == 0 ) ? m_cEncCfg.m_GOPList[ 0 ].m_POC : poc + m_nextPocOffset[ poc % m_cEncCfg.m_GOPSize ];
  if ( flush )
  {
    while( next >= max )
    {
      next += m_nextPocOffset[ next % m_cEncCfg.m_GOPSize ];
      CHECK( chk++ > m_cEncCfg.m_GOPSize, "error: next poc runs out of bounds" );
    }
  }
  return next;
}


/**
 - Application has picture buffer list with size of GOP + 1
 - Picture buffer list acts like as ring buffer
 - End of the list has the latest picture
 .
 \retval pic obtained picture buffer
 */
Picture* EncLib::xGetNewPicBuffer( const PPS& pps, const SPS& sps )
{
  Slice::sortPicList( m_cListPic );

  Picture* pic = nullptr;

  // use an entry in the buffered list if the maximum number that need buffering has been reached:
  if ( (int)m_cListPic.size() >= ( m_cEncCfg.m_InputQueueSize + m_cEncCfg.m_maxDecPicBuffering[ MAX_TLAYER - 1 ] + 2 ) )
  {
    auto picItr = std::begin( m_cListPic );
    while ( picItr != std::end( m_cListPic ) )
    {
      Picture* curPic = *picItr;
      if ( ! curPic->isNeededForOutput && ! curPic->isReferenced && curPic->refCounter <= 0 )
      {
        pic = curPic;
        break;
      }
      picItr++;
    }

    CHECK( pic == nullptr, "Error: no free entry in picture list found" );

    // if PPS ID is the same, we will assume that it has not changed since it was last used and return the old object.
    if ( pps.ppsId != pic->cs->pps->ppsId )
    {
      // the IDs differ - free up an entry in the list, and then create a new one, as with the case where the max buffering state has not been reached.
      m_cListPic.erase( picItr );
      pic->destroy();
      delete pic;
      pic = nullptr;
    }
  }

  if ( pic == nullptr )
  {
    const int padding = m_cEncCfg.m_MCTF ? MCTF_PADDING : 0;
    pic = new Picture;
    pic->create( sps.chromaFormatIdc, Size( pps.picWidthInLumaSamples, pps.picHeightInLumaSamples), sps.CTUSize, sps.CTUSize+16, false, padding );
    m_cListPic.push_back( pic );
  }

  pic->isMctfFiltered    = false;
  pic->isInitDone        = false;
  pic->isReconstructed   = false;
  pic->isBorderExtended  = false;
  pic->isReferenced      = true;
  pic->isNeededForOutput = true;
  pic->writePic          = false;
  pic->encPic            = false;
  pic->refCounter        = 0;
  pic->poc               = -1;

  pic->encTime.resetTimer();

  return pic;
}

void EncLib::xInitPicture( Picture& pic, int picNum, const PPS& pps, const SPS& sps, const VPS& vps, const DCI& dci )
{
  const int gopId = xGetGopIdFromPoc( picNum );

  pic.poc    = picNum;
  pic.gopId  = gopId;
  pic.TLayer = m_cEncCfg.m_GOPList[ pic.gopId ].m_temporalId;

  std::mutex* mutex = ( m_cEncCfg.m_frameParallel ) ? &m_unitCacheMutex : nullptr;

  PicHeader *picHeader = nullptr;
  if ( pic.cs && pic.cs->picHeader )
  {
    delete pic.cs->picHeader;
    pic.cs->picHeader = nullptr;
  }

  pic.finalInit( vps, sps, pps, *picHeader, m_shrdUnitCache, mutex, nullptr, nullptr );

  pic.vps = &vps;
  pic.dci = &dci;

  pic.createTempBuffers( pic.cs->pps->pcv->maxCUSize );
  pic.cs->createCoeffs();

  // filter data initialization
  const uint32_t numberOfCtusInFrame = pic.cs->pcv->sizeInCtus;

  if (m_cEncCfg.m_usePerceptQPA)
  {
    pic.ctuQpaLambda.resize (numberOfCtusInFrame);
    pic.ctuAdaptedQP.resize (numberOfCtusInFrame);
  }

  if ( pic.cs->sps->saoEnabled )
  {
    pic.resizeSAO( numberOfCtusInFrame, 0 );
    pic.resizeSAO( numberOfCtusInFrame, 1 );
  }

  if ( pic.cs->sps->alfEnabled )
  {
    pic.resizeAlfCtuEnabled( numberOfCtusInFrame );
    pic.resizeAlfCtuAlternative( numberOfCtusInFrame );
    pic.resizeAlfCtbFilterIndex( numberOfCtusInFrame );
  }
}

void EncLib::xDeletePicBuffer()
{
  PicList::iterator iterPic = m_cListPic.begin();
  int iSize = int( m_cListPic.size() );

  for ( int i = 0; i < iSize; i++ )
  {
    Picture* pic = *(iterPic++);

    if ( pic->cs && pic->cs->picHeader )
    {
      delete pic->cs->picHeader;
      pic->cs->picHeader = nullptr;
    }
    pic->destroy();

    delete pic;
    pic = nullptr;
  }
}

Picture* EncLib::xGetPictureBuffer( int poc )
{
  for ( auto& picItr : m_cListPic )
  {
    if ( picItr->poc == poc )
    {
      return picItr;
    }
  }

  THROW( "Error: picture not found in list");
  return nullptr;
}

void EncLib::xCreateCodingOrder( int start, int max, int numInQueue, bool flush, std::vector<Picture*>& encList )
{
  encList.clear();
  int poc = start;
  int num = 0;
  while ( poc < max )
  {
    Picture* pic = xGetPictureBuffer( poc );
    if ( m_cEncCfg.m_MCTF && ! pic->isMctfFiltered )
    {
      break;
    }
    encList.push_back( pic );
    num += 1;
    if ( num >= numInQueue )
    {
      break;
    }
    poc = xGetNextPocICO( poc, flush, max );
  }
  CHECK( encList.size() == 0, "error: no pictures to be encoded found" );
}

void EncLib::xInitVPS(VPS &vps) const
{
  // The SPS must have already been set up.
  // set the VPS profile information.
  vps.maxLayers                   = 1;
  vps.maxSubLayers                = 1;
  vps.vpsId                       = 0;
  vps.allLayersSameNumSubLayers   = true;
  vps.allIndependentLayers        = true;
  vps.eachLayerIsAnOls            = true;
  vps.olsModeIdc                  = 0;
  vps.numOutputLayerSets          = 1;
  vps.numPtls                     = 1;
  vps.extension                   = false;
  vps.totalNumOLSs                = 0;
  vps.numDpbParams                = 0;
  vps.sublayerDpbParamsPresent    = false;
  vps.targetOlsIdx                = -1;

  for (int i = 0; i < MAX_VPS_LAYERS; i++)
  {
    vps.layerId[i]                = 0;
    vps.independentLayer[i]       = true;
    for (int j = 0; j < MAX_VPS_LAYERS; j++)
    {
      vps.directRefLayer[i][j]    = 0;
      vps.directRefLayerIdx[i][j] = MAX_VPS_LAYERS;
      vps.interLayerRefIdx[i][i]  = NOT_VALID;
    }
  }

  for (int i = 0; i < MAX_NUM_OLSS; i++)
  {
    for (int j = 0; j < MAX_VPS_LAYERS; j++)
    {
      vps.olsOutputLayer[i][j]    = 0;
    }
    vps.ptPresent[i]              = (i == 0) ? 1 : 0;
    vps.ptlMaxTemporalId[i]       = vps.maxSubLayers - 1;
    vps.olsPtlIdx[i]              = 0;
  }

  vps.profileTierLevel.resize( 1 );
}

void EncLib::xInitDCI(DCI &dci, const SPS &sps, const int dciId) const
{
  // The SPS must have already been set up.
  // set the DPS profile information.
  dci.dciId                 = dciId;
  dci.maxSubLayersMinus1    = sps.maxTLayers - 1;

  dci.profileTierLevel.resize(1);
  // copy profile level tier info
  dci.profileTierLevel[0]   = sps.profileTierLevel;
}

void EncLib::xInitConstraintInfo(ConstraintInfo &ci) const
{
  bool hasNonZeroTemporalId = false;
  bool hasLeadingPictures   = false;
  for (unsigned int i = 0; i < m_cEncCfg.m_GOPSize; i++)
  {
    if ( m_cEncCfg.m_GOPList[i].m_temporalId != 0 )
    {
      hasNonZeroTemporalId = true;
    }
    for( int l = 0; l < 2; l++ )
    {
      for ( unsigned int j = 0; !hasLeadingPictures && j < m_cEncCfg.m_GOPList[i].m_numRefPics[l]; j++)
      {
        if ( m_cEncCfg.m_GOPList[i].m_deltaRefPics[l][j] < 0 )
        {
          hasLeadingPictures = true;
        }
      }
    }
  }

  ci.intraOnlyConstraintFlag                      = m_cEncCfg.m_intraOnlyConstraintFlag;
  ci.maxBitDepthConstraintIdc                     = m_cEncCfg.m_bitDepthConstraintValue - 8;
  ci.maxChromaFormatConstraintIdc                 = m_cEncCfg.m_internChromaFormat;
  ci.onePictureOnlyConstraintFlag             = false;
  ci.lowerBitRateConstraintFlag               = false;
  ci.allLayersIndependentConstraintFlag       = false;
  ci.noMrlConstraintFlag                      = false;
  ci.noIspConstraintFlag                      = false;
  ci.noMipConstraintFlag                      = false;
  ci.noLfnstConstraintFlag                    = false;
  ci.noMmvdConstraintFlag                     = false;
  ci.noSmvdConstraintFlag                     = false;
  ci.noProfConstraintFlag                     = false;
  ci.noPaletteConstraintFlag                  = false;
  ci.noActConstraintFlag                      = false;
  ci.noLmcsConstraintFlag                     = false;
  ci.noQtbttDualTreeIntraConstraintFlag           = ! m_cEncCfg.m_dualITree;
  ci.noPartitionConstraintsOverrideConstraintFlag = ! m_cEncCfg.m_useAMaxBT;
  ci.noSaoConstraintFlag                          = ! m_cEncCfg.m_bUseSAO;
  ci.noAlfConstraintFlag                          = ! m_cEncCfg.m_alf;
  ci.noCCAlfConstraintFlag                        = ! m_cEncCfg.m_ccalf;
  ci.noRefWraparoundConstraintFlag                = false;
  ci.noTemporalMvpConstraintFlag                  = m_cEncCfg.m_TMVPModeId == 0;
  ci.noSbtmvpConstraintFlag                       = !m_cEncCfg.m_SbTMVP;
  ci.noAmvrConstraintFlag                         = false;
  ci.noBdofConstraintFlag                         = ! m_cEncCfg.m_BDOF;
  ci.noDmvrConstraintFlag                         = ! m_cEncCfg.m_DMVR;
  ci.noCclmConstraintFlag                         = ! m_cEncCfg.m_LMChroma;
  ci.noMtsConstraintFlag                          = !(m_cEncCfg.m_MTSImplicit || m_cEncCfg.m_MTS);
  ci.noSbtConstraintFlag                          = m_cEncCfg.m_SBT == 0;
  ci.noAffineMotionConstraintFlag                 = ! m_cEncCfg.m_Affine;
  ci.noBcwConstraintFlag                          = true;
  ci.noIbcConstraintFlag                          = true;
  ci.noCiipConstraintFlag                         = m_cEncCfg.m_CIIP == 0;
  ci.noGeoConstraintFlag                          = m_cEncCfg.m_Geo == 0;
  ci.noLadfConstraintFlag                         = true;
  ci.noTransformSkipConstraintFlag                = true;
  ci.noBDPCMConstraintFlag                        = true;
  ci.noJointCbCrConstraintFlag                    = ! m_cEncCfg.m_JointCbCrMode;
  ci.noMrlConstraintFlag                          = ! m_cEncCfg.m_MRL;
  ci.noIspConstraintFlag                          = true;
  ci.noMipConstraintFlag                          = ! m_cEncCfg.m_MIP;
  ci.noQpDeltaConstraintFlag                      = false;
  ci.noDepQuantConstraintFlag                     = ! m_cEncCfg.m_DepQuantEnabled;
  ci.noMixedNaluTypesInPicConstraintFlag          = false;
  ci.noSignDataHidingConstraintFlag               = ! m_cEncCfg.m_SignDataHidingEnabled;
  ci.noLfnstConstraintFlag                        = ! m_cEncCfg.m_LFNST;
  ci.noMmvdConstraintFlag                         = ! m_cEncCfg.m_MMVD;
  ci.noSmvdConstraintFlag                         = ! m_cEncCfg.m_SMVD;
  ci.noProfConstraintFlag                         = ! m_cEncCfg.m_PROF;
  ci.noPaletteConstraintFlag                      = true;
  ci.noActConstraintFlag                          = true;
  ci.noLmcsConstraintFlag                         = ! m_cEncCfg.m_lumaReshapeEnable;
  ci.noTrailConstraintFlag                        = m_cEncCfg.m_IntraPeriod == 1;
  ci.noStsaConstraintFlag                         = m_cEncCfg.m_IntraPeriod == 1 || !hasNonZeroTemporalId;
  ci.noRaslConstraintFlag                         = m_cEncCfg.m_IntraPeriod == 1 || !hasLeadingPictures;
  ci.noRadlConstraintFlag                         = m_cEncCfg.m_IntraPeriod == 1 || !hasLeadingPictures;
  ci.noIdrConstraintFlag                          = false;
  ci.noCraConstraintFlag                          = m_cEncCfg.m_DecodingRefreshType != 1;
  ci.noGdrConstraintFlag                          = false;
  ci.noApsConstraintFlag                          = ( !m_cEncCfg.m_alf && !m_cEncCfg.m_lumaReshapeEnable /*&& m_useScalingListId == SCALING_LIST_OFF*/);
}

void EncLib::xInitSPS(SPS &sps) const
{
  ProfileTierLevel* profileTierLevel = &sps.profileTierLevel;

  xInitConstraintInfo( profileTierLevel->constraintInfo );

  profileTierLevel->levelIdc      = m_cEncCfg.m_level;
  profileTierLevel->tierFlag      = m_cEncCfg.m_levelTier;
  profileTierLevel->profileIdc    = m_cEncCfg.m_profile;
  profileTierLevel->subProfileIdc.clear();
  profileTierLevel->subProfileIdc.push_back( m_cEncCfg.m_subProfile );

  sps.maxPicWidthInLumaSamples      = m_cEncCfg.m_SourceWidth;
  sps.maxPicHeightInLumaSamples     = m_cEncCfg.m_SourceHeight;
  sps.chromaFormatIdc               = m_cEncCfg.m_internChromaFormat;
  sps.CTUSize                       = m_cEncCfg.m_CTUSize;
  sps.maxMTTDepth[0]                = m_cEncCfg.m_maxMTTDepthI;
  sps.maxMTTDepth[1]                = m_cEncCfg.m_maxMTTDepth;
  sps.maxMTTDepth[2]                = m_cEncCfg.m_maxMTTDepthIChroma;
  for( int i = 0; i < 3; i++)
  {
    sps.minQTSize[i]                = m_cEncCfg.m_MinQT[i];
    sps.maxBTSize[i]                = m_cEncCfg.m_maxBT[i];
    sps.maxTTSize[i]                = m_cEncCfg.m_maxTT[i];
  }
  sps.minQTSize[2]                <<= getChannelTypeScaleX(CH_C, m_cEncCfg.m_internChromaFormat);

  sps.maxNumMergeCand               = m_cEncCfg.m_maxNumMergeCand;
  sps.maxNumAffineMergeCand         = m_cEncCfg.m_Affine ? m_cEncCfg.m_maxNumAffineMergeCand : 0;
  sps.maxNumGeoCand                 = m_cEncCfg.m_maxNumGeoCand;
  sps.maxNumIBCMergeCand            = 0; // m_cEncCfg.m_maxNumIBCMergeCand;

  sps.idrRefParamList               = m_cEncCfg.m_idrRefParamList;
  sps.dualITree                     = m_cEncCfg.m_dualITree;
  sps.MTS                           = m_cEncCfg.m_MTS || m_cEncCfg.m_MTSImplicit;
  sps.SMVD                          = m_cEncCfg.m_SMVD;
  sps.AMVR                          = m_cEncCfg.m_AMVRspeed != IMV_OFF;
  sps.LMChroma                      = m_cEncCfg.m_LMChroma;
  sps.horCollocatedChroma           = m_cEncCfg.m_horCollocatedChromaFlag;
  sps.verCollocatedChroma           = m_cEncCfg.m_verCollocatedChromaFlag;
  sps.BDOF                          = m_cEncCfg.m_BDOF;
  sps.DMVR                          = m_cEncCfg.m_DMVR;
  sps.lumaReshapeEnable             = m_cEncCfg.m_lumaReshapeEnable;
  sps.Affine                        = m_cEncCfg.m_Affine;
  sps.PROF                          = m_cEncCfg.m_PROF;
  sps.ProfPresent                   = m_cEncCfg.m_PROF;
  sps.AffineType                    = m_cEncCfg.m_AffineType;
  sps.MMVD                          = m_cEncCfg.m_MMVD != 0;
  sps.fpelMmvd                      = m_cEncCfg.m_allowDisFracMMVD;
  sps.GEO                           = m_cEncCfg.m_Geo != 0;
  sps.MIP                           = m_cEncCfg.m_MIP;
  sps.MRL                           = m_cEncCfg.m_MRL;
  sps.BdofPresent                   = m_cEncCfg.m_BDOF;
  sps.DmvrPresent                   = m_cEncCfg.m_DMVR;
  sps.partitionOverrideEnabled      = m_cEncCfg.m_useAMaxBT;
  sps.rprEnabled                    = true; //as in vtm
  sps.log2MinCodingBlockSize        = 2;
  sps.log2MaxTbSize                 = m_cEncCfg.m_log2MaxTbSize;
  sps.temporalMVPEnabled            = m_cEncCfg.m_TMVPModeId == 2 || m_cEncCfg.m_TMVPModeId == 1;
  sps.LFNST                         = m_cEncCfg.m_LFNST != 0;
  sps.entropyCodingSyncEnabled      = m_cEncCfg.m_entropyCodingSyncEnabled;
  sps.entryPointsPresent            = m_cEncCfg.m_entryPointsPresent;
  sps.depQuantEnabled               = m_cEncCfg.m_DepQuantEnabled;
  sps.signDataHidingEnabled         = m_cEncCfg.m_SignDataHidingEnabled;
  sps.MTSIntra                      = m_cEncCfg.m_MTS ;

  for (uint32_t chType = 0; chType < MAX_NUM_CH; chType++)
  {
    sps.bitDepths.recon[chType]     = m_cEncCfg.m_internalBitDepth[chType];
    sps.qpBDOffset[chType]          = 6 * (m_cEncCfg.m_internalBitDepth[chType] - 8);
    sps.internalMinusInputBitDepth[chType] = (m_cEncCfg.m_internalBitDepth[chType] - m_cEncCfg.m_inputBitDepth[chType]);
  }

  sps.alfEnabled                    = m_cEncCfg.m_alf;
  sps.ccalfEnabled                  = m_cEncCfg.m_ccalf;

  sps.saoEnabled                    = m_cEncCfg.m_bUseSAO;
  sps.jointCbCr                     = m_cEncCfg.m_JointCbCrMode;
  sps.maxTLayers                    = m_cEncCfg.m_maxTempLayer;
  sps.rpl1CopyFromRpl0              = m_cEncCfg.m_IntraPeriod < 0;
  sps.SbtMvp                        = m_cEncCfg.m_SbTMVP;
  sps.CIIP                          = m_cEncCfg.m_CIIP != 0;
  sps.SBT                           = m_cEncCfg.m_SBT != 0;

  for (int i = 0; i < std::min(sps.maxTLayers, (uint32_t) MAX_TLAYER); i++ )
  {
    sps.maxDecPicBuffering[i]       = m_cEncCfg.m_maxDecPicBuffering[i];
    sps.numReorderPics[i]           = m_cEncCfg.m_numReorderPics[i];
  }

  sps.vuiParametersPresent          = m_cEncCfg.m_vuiParametersPresent;

  if (sps.vuiParametersPresent)
  {
    VUI& vui = sps.vuiParameters;
    vui.aspectRatioInfoPresent        = m_cEncCfg.m_aspectRatioInfoPresent;
    vui.aspectRatioIdc                = m_cEncCfg.m_aspectRatioIdc;
    vui.sarWidth                      = m_cEncCfg.m_sarWidth;
    vui.sarHeight                     = m_cEncCfg.m_sarHeight;
    vui.colourDescriptionPresent      = m_cEncCfg.m_colourDescriptionPresent;
    vui.colourPrimaries               = m_cEncCfg.m_colourPrimaries;
    vui.transferCharacteristics       = m_cEncCfg.m_transferCharacteristics;
    vui.matrixCoefficients            = m_cEncCfg.m_matrixCoefficients;
    vui.chromaLocInfoPresent          = m_cEncCfg.m_chromaLocInfoPresent;
    vui.chromaSampleLocTypeTopField   = m_cEncCfg.m_chromaSampleLocTypeTopField;
    vui.chromaSampleLocTypeBottomField= m_cEncCfg.m_chromaSampleLocTypeBottomField;
    vui.chromaSampleLocType           = m_cEncCfg.m_chromaSampleLocType;
    vui.overscanInfoPresent           = m_cEncCfg.m_overscanInfoPresent;
    vui.overscanAppropriateFlag       = m_cEncCfg.m_overscanAppropriateFlag;
    vui.videoFullRangeFlag            = m_cEncCfg.m_videoFullRangeFlag;
  }

  sps.numLongTermRefPicSPS            = NUM_LONG_TERM_REF_PIC_SPS;
  CHECK(!(NUM_LONG_TERM_REF_PIC_SPS <= MAX_NUM_LONG_TERM_REF_PICS), "Unspecified error");
  for (int k = 0; k < NUM_LONG_TERM_REF_PIC_SPS; k++)
  {
    sps.ltRefPicPocLsbSps[k]          = 0;
    sps.usedByCurrPicLtSPS[k]         = 0;
  }
  sps.chromaQpMappingTable.m_numQpTables = (m_cEncCfg.m_chromaQpMappingTableParams.m_sameCQPTableForAllChromaFlag ? 1 : (sps.jointCbCr ? 3 : 2));
  sps.chromaQpMappingTable.setParams(m_cEncCfg.m_chromaQpMappingTableParams, sps.qpBDOffset[ CH_C ]);
  sps.chromaQpMappingTable.derivedChromaQPMappingTables();
}

void EncLib::xInitPPS(PPS &pps, const SPS &sps) const
{
  bool bUseDQP = m_cEncCfg.m_cuQpDeltaSubdiv > 0;
  bUseDQP |= m_cEncCfg.m_lumaLevelToDeltaQPEnabled;
  bUseDQP |= m_cEncCfg.m_usePerceptQPA && (m_cEncCfg.m_QP <= MAX_QP_PERCEPT_QPA);

  if (m_cEncCfg.m_costMode==COST_SEQUENCE_LEVEL_LOSSLESS || m_cEncCfg.m_costMode==COST_LOSSLESS_CODING)
  {
    bUseDQP = false;
  }


  // pps ID already initialised.
  pps.spsId                         = sps.spsId;
  pps.jointCbCrQpOffsetPresent      = m_cEncCfg.m_JointCbCrMode;
  pps.picWidthInLumaSamples         = m_cEncCfg.m_SourceWidth;
  pps.picHeightInLumaSamples        = m_cEncCfg.m_SourceHeight;
  pps.conformanceWindow.setWindow( m_cEncCfg.m_confWinLeft, m_cEncCfg.m_confWinRight, m_cEncCfg.m_confWinTop, m_cEncCfg.m_confWinBottom );

  pps.picWidthInCtu                 = (pps.picWidthInLumaSamples + (sps.CTUSize-1)) / sps.CTUSize;
  pps.picHeightInCtu                = (pps.picHeightInLumaSamples + (sps.CTUSize-1)) / sps.CTUSize;
  pps.subPics.clear();
  pps.subPics.resize(1);
  pps.subPics[0].init( pps.picWidthInCtu, pps.picHeightInCtu, pps.picWidthInLumaSamples, pps.picHeightInLumaSamples);
  pps.noPicPartition                = true;
  pps.useDQP                        = m_cEncCfg.m_RCRateControlMode ? true : bUseDQP;

  if ( m_cEncCfg.m_cuChromaQpOffsetSubdiv >= 0 )
  {
//th check how this is configured now    pps.cuChromaQpOffsetSubdiv = m_cEncCfg.m_cuChromaQpOffsetSubdiv;
    pps.chromaQpOffsetListLen = 0;
    pps.setChromaQpOffsetListEntry(1, 6, 6, 6);
  }

  {
    int baseQp = m_cEncCfg.m_QP-26;
    if( 16 == m_cEncCfg.m_GOPSize )
    {
      baseQp += 2;
    }

    const int maxDQP = 37;
    const int minDQP = -26 + sps.qpBDOffset[ CH_L ];
    pps.picInitQPMinus26 = std::min( maxDQP, std::max( minDQP, baseQp ) );
  }

  if (m_cEncCfg.m_wcgChromaQpControl.enabled )
  {
    const int baseQp      = m_cEncCfg.m_QP + pps.ppsId;
    const double chromaQp = m_cEncCfg.m_wcgChromaQpControl.chromaQpScale * baseQp + m_cEncCfg.m_wcgChromaQpControl.chromaQpOffset;
    const double dcbQP    = m_cEncCfg.m_wcgChromaQpControl.chromaCbQpScale * chromaQp;
    const double dcrQP    = m_cEncCfg.m_wcgChromaQpControl.chromaCrQpScale * chromaQp;
    const int cbQP        = std::min(0, (int)(dcbQP + ( dcbQP < 0 ? -0.5 : 0.5) ));
    const int crQP        = std::min(0, (int)(dcrQP + ( dcrQP < 0 ? -0.5 : 0.5) ));
    pps.chromaQpOffset[COMP_Y]          = 0;
    pps.chromaQpOffset[COMP_Cb]         = Clip3( -12, 12, cbQP + m_cEncCfg.m_chromaCbQpOffset);
    pps.chromaQpOffset[COMP_Cr]         = Clip3( -12, 12, crQP + m_cEncCfg.m_chromaCrQpOffset);
    pps.chromaQpOffset[COMP_JOINT_CbCr] = Clip3( -12, 12, ( cbQP + crQP ) / 2 + m_cEncCfg.m_chromaCbCrQpOffset);
  }
  else
  {
    pps.chromaQpOffset[COMP_Y]          = 0;
    pps.chromaQpOffset[COMP_Cb]         = m_cEncCfg.m_chromaCbQpOffset;
    pps.chromaQpOffset[COMP_Cr]         = m_cEncCfg.m_chromaCrQpOffset;
    pps.chromaQpOffset[COMP_JOINT_CbCr] = m_cEncCfg.m_chromaCbCrQpOffset;
  }

  bool bChromaDeltaQPEnabled = false;
  {
    bChromaDeltaQPEnabled = ( m_cEncCfg.m_sliceChromaQpOffsetIntraOrPeriodic[ 0 ] || m_cEncCfg.m_sliceChromaQpOffsetIntraOrPeriodic[ 1 ] );
    bChromaDeltaQPEnabled     |= ((m_cEncCfg.m_usePerceptQPA > 0 && m_cEncCfg.m_usePerceptQPA <= 4) || m_cEncCfg.m_sliceChromaQpOffsetPeriodicity > 0) && (m_cEncCfg.m_internChromaFormat != CHROMA_400);
    if ( !bChromaDeltaQPEnabled && sps.dualITree && ( m_cEncCfg.m_internChromaFormat != CHROMA_400) )
    {
      bChromaDeltaQPEnabled = (m_cEncCfg.m_chromaCbQpOffsetDualTree != 0 || m_cEncCfg.m_chromaCrQpOffsetDualTree != 0 || m_cEncCfg.m_chromaCbCrQpOffsetDualTree != 0);
    }

    for( int i = 0; !bChromaDeltaQPEnabled && i < m_cEncCfg.m_GOPSize; i++ )
    {
      if( m_cEncCfg.m_GOPList[ i ].m_CbQPoffset || m_cEncCfg.m_GOPList[ i ].m_CrQPoffset )
      {
        bChromaDeltaQPEnabled = true;
      }
    }
  }
  pps.sliceChromaQpFlag                 = bChromaDeltaQPEnabled;
  pps.outputFlagPresent                 = false;
  pps.deblockingFilterOverrideEnabled   = !m_cEncCfg.m_loopFilterOffsetInPPS;
  pps.deblockingFilterDisabled          = m_cEncCfg.m_bLoopFilterDisable;

  if (! pps.deblockingFilterDisabled)
  {
    for( int comp = 0; comp < MAX_NUM_COMP; comp++)
    {
      pps.deblockingFilterBetaOffsetDiv2[comp]  = m_cEncCfg.m_loopFilterBetaOffsetDiv2[comp];
      pps.deblockingFilterTcOffsetDiv2[comp]    = m_cEncCfg.m_loopFilterTcOffsetDiv2[comp];
    }
  }

  // deblockingFilterControlPresent is true if any of the settings differ from the inferred values:
  bool deblockingFilterControlPresent   = pps.deblockingFilterOverrideEnabled ||
                                          pps.deblockingFilterDisabled     ||
                                          pps.deblockingFilterBetaOffsetDiv2[COMP_Y] != 0 ||
                                          pps.deblockingFilterTcOffsetDiv2  [COMP_Y] != 0 ||
                                          pps.deblockingFilterBetaOffsetDiv2[COMP_Cb] != 0 ||
                                          pps.deblockingFilterTcOffsetDiv2  [COMP_Cb] != 0 ||
                                          pps.deblockingFilterBetaOffsetDiv2[COMP_Cr] != 0 ||
                                          pps.deblockingFilterTcOffsetDiv2  [COMP_Cr] != 0;

  pps.deblockingFilterControlPresent    = deblockingFilterControlPresent;
  pps.cabacInitPresent                  = m_cEncCfg.m_cabacInitPresent;
  pps.loopFilterAcrossSlicesEnabled     = m_cEncCfg.m_bLFCrossSliceBoundaryFlag;
  pps.rpl1IdxPresent                    = sps.rpl1IdxPresent;

  const uint32_t chromaArrayType = (int)sps.separateColourPlane ? CHROMA_400 : sps.chromaFormatIdc;
  if( chromaArrayType != CHROMA_400  )
  {
    bool chromaQPOffsetNotZero = ( pps.chromaQpOffset[COMP_Cb] != 0 || pps.chromaQpOffset[COMP_Cr] != 0 || pps.jointCbCrQpOffsetPresent || pps.sliceChromaQpFlag || pps.chromaQpOffsetListLen );
    bool chromaDbfOffsetNotAsLuma = ( pps.deblockingFilterBetaOffsetDiv2[COMP_Cb] != pps.deblockingFilterBetaOffsetDiv2[COMP_Y] 
                                   || pps.deblockingFilterBetaOffsetDiv2[COMP_Cr] != pps.deblockingFilterBetaOffsetDiv2[COMP_Y]
                                   || pps.deblockingFilterTcOffsetDiv2[COMP_Cb] != pps.deblockingFilterTcOffsetDiv2[COMP_Y]  
                                   || pps.deblockingFilterTcOffsetDiv2[COMP_Cr] != pps.deblockingFilterTcOffsetDiv2[COMP_Y]);
    pps.usePPSChromaTool = chromaQPOffsetNotZero || chromaDbfOffsetNotAsLuma;
  }

  int histogram[MAX_NUM_REF + 1];
  for( int i = 0; i <= MAX_NUM_REF; i++ )
  {
    histogram[i]=0;
  }
  for( int i = 0; i < m_cEncCfg.m_GOPSize; i++)
  {
    CHECK(!(m_cEncCfg.m_RPLList0[ i ].m_numRefPicsActive >= 0 && m_cEncCfg.m_RPLList0[ i ].m_numRefPicsActive <= MAX_NUM_REF), "Unspecified error");
    histogram[m_cEncCfg.m_RPLList0[ i ].m_numRefPicsActive]++;
  }

  int maxHist=-1;
  int bestPos=0;
  for( int i = 0; i <= MAX_NUM_REF; i++ )
  {
    if(histogram[i]>maxHist)
    {
      maxHist=histogram[i];
      bestPos=i;
    }
  }
  CHECK(!(bestPos <= 15), "Unspecified error");
  pps.numRefIdxL0DefaultActive = bestPos;
  pps.numRefIdxL1DefaultActive = bestPos;

  xInitPPSforTiles(pps);

  pps.pcv            = new PreCalcValues( sps, pps, true );
}

void EncLib::xInitRPL(SPS &sps) const
{
  const int numRPLCandidates = m_cEncCfg.m_numRPLList0;
  sps.rplList[0].resize(numRPLCandidates+1);
  sps.rplList[1].resize(numRPLCandidates+1);
  sps.rpl1IdxPresent = (sps.rplList[0].size() != sps.rplList[1].size());

  for (int i = 0; i < 2; i++)
  {
    const RPLEntry* rplCfg = ( i == 0 ) ? m_cEncCfg.m_RPLList0 : m_cEncCfg.m_RPLList1;
    for (int j = 0; j < numRPLCandidates; j++)
    {
      const RPLEntry &ge = rplCfg[ j ];
      ReferencePictureList&rpl = sps.rplList[i][j];
      rpl.numberOfShorttermPictures = ge.m_numRefPics;
      rpl.numberOfLongtermPictures = 0;   //Hardcoded as 0 for now. need to update this when implementing LTRP
      rpl.numberOfActivePictures = ge.m_numRefPicsActive;

      for (int k = 0; k < ge.m_numRefPics; k++)
      {
        rpl.setRefPicIdentifier(k, -ge.m_deltaRefPics[k], 0, false, 0);
      }
    }
  }

  //Check if all delta POC of STRP in each RPL has the same sign
  //Check RPLL0 first
  bool isAllEntriesinRPLHasSameSignFlag = true;
  for( int list = 0; list < 2; list++)
  {
    const RPLList& rplList = sps.rplList[list];
    uint32_t numRPL        = (uint32_t)rplList.size();

    bool isFirstEntry = true;
    bool lastSign = true;        //true = positive ; false = negative
    for (uint32_t ii = 0; isAllEntriesinRPLHasSameSignFlag && ii < numRPL; ii++)
    {
      const ReferencePictureList& rpl = rplList[ii];
      for (uint32_t jj = 0; jj < rpl.numberOfActivePictures; jj++)
      {
        if(rpl.isLongtermRefPic[jj])
          continue;

        if( isFirstEntry )
        {
          lastSign = (rpl.refPicIdentifier[jj] >= 0) ? true : false;
          isFirstEntry = false;
        }
        else
        {
          int ref = ( jj == 0 && !isFirstEntry ) ? 0 : rpl.refPicIdentifier[jj-1];
          if (((rpl.refPicIdentifier[jj] - ref) >= 0 ) != lastSign)
          {
            isAllEntriesinRPLHasSameSignFlag = false;
            break;  // break the inner loop
          }
        }
      }
    }
  }
    
  sps.allRplEntriesHasSameSign = isAllEntriesinRPLHasSameSignFlag;

  bool isRpl1CopiedFromRpl0 = true;
  for( int i = 0; isRpl1CopiedFromRpl0 && i < numRPLCandidates; i++)
  {
    if( sps.rplList[0][i].getNumRefEntries() == sps.rplList[1][i].getNumRefEntries() )
    {
      for( int j = 0; isRpl1CopiedFromRpl0 && j < sps.rplList[0][i].getNumRefEntries(); j++ )
      {
        if( sps.rplList[0][i].refPicIdentifier[j] != sps.rplList[1][i].refPicIdentifier[j] )
        {
          isRpl1CopiedFromRpl0 = false;
        }
      }
    }
    else
    {
      isRpl1CopiedFromRpl0 = false;
    }
  }
  sps.rpl1CopyFromRpl0 = isRpl1CopiedFromRpl0;
}

void EncLib::xInitPPSforTiles(PPS &pps) const
{
  pps.sliceMap.clear();
  pps.sliceMap.resize(1);
  pps.sliceMap[0].addCtusToSlice(0, pps.picWidthInCtu, 0, pps.picHeightInCtu, pps.picWidthInCtu);
  pps.ctuToTileCol.resize(pps.picWidthInCtu, 0);
  pps.ctuToTileRow.resize(pps.picHeightInCtu, 0);
}

void EncLib::xOutputRecYuv()
{
  Slice::sortPicList( m_cListPic );

  for ( const auto& picItr : m_cListPic )
  {
    if ( picItr->poc < m_pocRecOut )
      continue;
    if ( ! picItr->isReconstructed || picItr->poc != m_pocRecOut )
      return;
    const PPS& pps = *(picItr->cs->pps);
    YUVBuffer yuvBuffer;
    setupYuvBuffer( picItr->getRecoBuf(), yuvBuffer, &pps.conformanceWindow );
    if ( m_yuvWriterIf )
    {
      m_yuvWriterIf->outputYuv( yuvBuffer );
    }
    m_pocRecOut = picItr->poc + 1;
    picItr->isNeededForOutput = false;
  }
}

} // namespace vvenc

//! \}

