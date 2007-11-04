// __BEGIN_LICENSE__
// 
// Copyright (C) 2006 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
// 
// Copyright 2006 Carnegie Mellon University. All rights reserved.
// 
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
// 
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
// 
// __END_LICENSE__

/// \file Interest.h
/// 
/// Basic classes and functions for calculating interest images.
/// 
#ifndef __VW_INTERESTPOINT_INTERESTVIEW_H__
#define __VW_INTERESTPOINT_INTERESTVIEW_H__

// STL Headers
#include <vector>

// Vision Workbench Headers
#include <vw/InterestPoint/InterestTraits.h>
#include <vw/InterestPoint/InterestData.h>

namespace vw { 
namespace ip {

  // --------------------------- ------------------------ ----------------------------
  // --------------------------- Harris Interest Operator ----------------------------
  // --------------------------- ------------------------ ----------------------------

  /// Harris Corner interest operator
  class HarrisInterestOperator {
    
    static const float DEFAULT_INTEREST_THRESHOLD = 0.03;
    double m_k, m_threshold;
    
  public:
    template <class ViewT> struct ViewType { 
      typedef ImageViewRef<typename ViewT::pixel_type> type; 
    };

    HarrisInterestOperator(float threshold = 1e-5, float k = -1.0) : m_k(k), m_threshold(threshold) {}

    /// Returns "cornerness" image, where the local maxima correspond to corners.
    /// By default uses Noble measure of corner strength (requires no tuning).
    /// Also supports Harris measure if positive k is specified (typical values:
    /// 0.04 <= k <= 0.15).
    template <class DataT>
    inline void operator() (DataT& data, float scale = 1.0) const {
      typedef typename DataT::source_type::pixel_type pixel_type;

      // Calculate elements of Harris matrix
      std::vector<float> kernel;
      generate_gaussian_kernel(kernel, scale, 0);
      ImageView<pixel_type> Ix2 = separable_convolution_filter(data.gradient_x() * data.gradient_x(),
                                                               kernel, kernel);
      ImageView<pixel_type> Iy2 = separable_convolution_filter(data.gradient_y() * data.gradient_y(),
                                                               kernel, kernel);
      ImageView<pixel_type> Ixy = separable_convolution_filter(data.gradient_x() * data.gradient_y(),
                                                               kernel, kernel);
      
      // Estimate "cornerness"
      ImageView<pixel_type> trace = Ix2 + Iy2;
      ImageView<pixel_type> det = Ix2 * Iy2 - Ixy * Ixy;
      if (m_k < 0) {
        // Noble measure (preferred)
        data.set_interest(det / (trace + 0.000001));
      } else {
        // Standard Harris corner measure
        data.set_interest(det - m_k * trace * trace);
      }
    }

    template <class ViewT>
    inline ImageViewRef<typename ViewT::pixel_type>
    operator() (ImageViewBase<ViewT> const& source, float scale = 1.0) const {
      ImageInterestData<ViewT, HarrisInterestOperator> data(source.impl());
      this->operator()(data, scale);
      return data.interest;      
    }

    template <class DataT>
    inline bool threshold (InterestPoint const& pt, DataT const& data) const {
      return (pt.interest > m_threshold);
    }
  };

  /// Type traits for Harris interest
  template <> struct InterestPeakType <HarrisInterestOperator> { static const int peak_type = IP_MAX; };
  

//   /// Harris interest measure uses the image gradients.
//   template <class SrcT> 
//   struct InterestOperatorTraits<SrcT, HarrisInterestOperator> {
//     typedef typename DefaultRasterizeT<SrcT>::type          rasterize_type;
//     typedef ImageView<typename SrcT::pixel_type>            gradient_type;
//     typedef typename DefaultMagT<SrcT>::type                mag_type;
//     typedef typename DefaultOriT<SrcT>::type                ori_type;
//     typedef rasterize_type                                  interest_type;
//   };

  // --------------------------- ------------------------ ----------------------------
  // --------------------- Laplacian of Gaussian Interest Operator -------------------
  // --------------------------- ------------------------ ----------------------------

  /// Log interest functor
  class LogInterestOperator {
    
    static const float DEFAULT_INTEREST_THRESHOLD = 0.03;
    double m_threshold;
    
  public:
    template <class ViewT> struct ViewType { 
      typedef ImageViewRef<typename ViewT::pixel_type> type; 
    };

    LogInterestOperator(float threshold = 0.03) : m_threshold(threshold) {}

    template <class ViewT>
    inline ImageViewRef<typename ViewT::pixel_type>
    operator() (ImageViewBase<ViewT> const& source, float scale = 1.0) const {
      return scale * laplacian_filter(source.impl());
    }

    // TODO: this should return something
    template <class DataT>
    inline void operator() (DataT& data, float scale = 1.0) const {
      data.set_interest(ImageViewRef<typename DataT::source_type::pixel_type>( scale * laplacian_filter(data.source())));
    }

    template <class DataT>
    inline bool threshold (InterestPoint const& pt, DataT const& data) const {
      return (fabs(pt.interest) > m_threshold);
    }
  };

  /// Type traits for Log interest
  template <> struct InterestPeakType <LogInterestOperator> { static const int peak_type = IP_MINMAX; };

}} //namespace vw::ip

#endif // __VW_INTERESTPOINT_INTERESTVIEW_H__
