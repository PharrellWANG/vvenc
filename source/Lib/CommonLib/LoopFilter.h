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
/** \file     LoopFilter.h
    \brief    deblocking filter (header)
*/

#pragma once

#include "CommonDef.h"
#include "Unit.h"
#include "Picture.h"

//! \ingroup CommonLib
//! \{

namespace vvenc {


#define DEBLOCK_SMALLEST_BLOCK  8

// ====================================================================================================================
// Class definition
// ====================================================================================================================

/// deblocking filter class
class LoopFilter
{
private:
  template<DeblockEdgeDir edgeDir>
  void xEdgeFilterLuma            ( CodingStructure& cs, const Position& pos, const LoopFilterParam& lfp, PelUnitBuf& picReco ) const;
  template<DeblockEdgeDir edgeDir>
  void xEdgeFilterChroma          ( CodingStructure& cs, const Position& pos, const LoopFilterParam& lfp, PelUnitBuf& picReco ) const;

  static const uint16_t sm_tcTable  [MAX_QP + 3];
  static const uint8_t  sm_betaTable[MAX_QP + 1];

  void( *xPelFilterLuma  )( Pel* piSrc, const ptrdiff_t step, const ptrdiff_t offset, const int tc, const bool sw, const int iThrCut, const bool bFilterSecondP, const bool bFilterSecondQ, const ClpRng& clpRng );
  void( *xFilteringPandQ )( Pel* src, ptrdiff_t step, const ptrdiff_t offset, int numberPSide, int numberQSide, int tc );

#ifdef TARGET_SIMD_X86
  void initLoopFilterX86();
  template <X86_VEXT vext>
  void _initLoopFilterX86();
#endif

public:

  LoopFilter();
  ~LoopFilter();

  /// picture-level deblocking filter
  void loopFilterPic                  ( CodingStructure& cs, bool calcFilterStrength ) const;
  void loopFilterPicLine              ( CodingStructure& cs, const ChannelType chType,                   const int ctuLine, const int offset = 0, DeblockEdgeDir edgeDir = NUM_EDGE_DIR ) const;
  void loopFilterCTU                  ( CodingStructure& cs, const ChannelType chType, const int ctuCol, const int ctuLine, const int offset = 0, DeblockEdgeDir edgeDir = NUM_EDGE_DIR ) const;
  static void calcFilterStrengthsCTU  ( CodingStructure& cs, const UnitArea& ctuArea, const bool clearLFP );

  static void calcFilterStrengths     ( const CodingUnit& cu, bool clearLF = false );

  static void getMaxFilterLength      ( const CodingUnit& cu, int& maxFilterLenghtLumaHor, int& maxFilterLenghtLumaVer );

  /// ctu-level deblocking filter
  template<DeblockEdgeDir edgeDir>
  void xDeblockArea                   ( CodingStructure& cs, const UnitArea& area, const ChannelType chType, PelUnitBuf& picReco ) const;

  void loopFilterCu                   ( const CodingUnit& cu, ChannelType chType, DeblockEdgeDir edgeDir, PelUnitBuf& dbBuffer );
};

} // namespace vvenc

//! \}

