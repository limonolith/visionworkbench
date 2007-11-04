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

/// \file Detector.h
/// 
/// Built-in classes and functions for performing interest point detection.
/// 
#ifndef __VW_INTERESTPOINT_DETECTOR_H__
#define __VW_INTERESTPOINT_DETECTOR_H__

#include <vw/InterestPoint/InterestData.h>
#include <vw/InterestPoint/Extrema.h>
#include <vw/InterestPoint/Localize.h>
#include <vw/InterestPoint/InterestOperator.h> 
#include <vw/InterestPoint/WeightedHistogram.h>
#include <vw/InterestPoint/ImageOctaveHistory.h>
#include <vw/Image/Algorithms.h>
#include <vw/Image/Manipulation.h>
#include <vw/FileIO.h>

namespace vw {
namespace ip {

  /// A CRTP InterestDetector base class.
  ///
  /// Subclasses of InterestDetectorBase must provide an
  /// implementation that detects interest points in an image region.
  /// The call operator() above calls this method, passing in each
  /// individual patch to be processed. That declaration is expected
  /// to be of the form:
  ///
  ///    template <class ViewT>
  ///    InterestPointList process_image(ImageViewBase<ViewT> const& image) const {
  ///
  template <class ImplT>
  struct InterestDetectorBase {

    /// \cond INTERNAL
    // Methods to access the derived type
    inline ImplT& impl() { return static_cast<ImplT&>(*this); }
    inline ImplT const& impl() const { return static_cast<ImplT const&>(*this); }
    /// \endcond

    /// Find the interest points in an image using the provided detector.  
    /// 
    /// Some images are too large to be processed for interest points all
    /// at once.  If the user specifies a max_interestpoint_image_dimension,
    /// this value is used to segment the image into smaller images which
    /// are passed individually to the interest point detector.  This routine
    /// combines the interest points from the sub-images once detection is
    /// complete.  Be aware that a few interest points along the segment
    /// borders may be lost.  A good max dimension depends on the amount
    /// of RAM needed by the detector (and the total RAM available).  A
    /// value of 2048 seems to work well in most cases.
    template <class ViewT>
    InterestPointList operator() (vw::ImageViewBase<ViewT> const& image,
                                  const int32 max_interestpoint_image_dimension = 0) {

      InterestPointList interest_points;
      
      vw_out(InfoMessage) << "\tFinding interest points" << std::flush;
      
      // If the user has not specified a chunk size, we process the
      // entire image in one shot.
      //
      // Note: We explicitly convert the image to PixelGray<float>
      // (rescaling as necessary) here before passing it off to the
      // rest of the interest detector code.
      if (!max_interestpoint_image_dimension) {
        vw_out(InfoMessage) << "..." << std::flush;
        interest_points = impl().process_image(pixel_cast<PixelGray<float> >(channel_cast_rescale<float>(image.impl())));
        
        // Otherwise we segment the image and process each sub-image
        // individually.
      } else {
        std::vector<BBox2i> bboxes = image_blocks(image.impl(), max_interestpoint_image_dimension, max_interestpoint_image_dimension);
        for (int i = 0; i < bboxes.size(); ++i) {
          vw_out(InfoMessage) << "." << std::flush;
          
          InterestPointList new_points = impl().process_image(crop(pixel_cast<PixelGray<float> >(channel_cast_rescale<float>(image.impl())), bboxes[i]));
          for (InterestPointList::iterator pt = new_points.begin(); pt != new_points.end(); ++pt) {
            (*pt).x +=  bboxes[i].min().x();
            (*pt).ix += bboxes[i].min().x();
            (*pt).y +=  bboxes[i].min().y();
            (*pt).iy += bboxes[i].min().y();
          }
          interest_points.splice(interest_points.end(), new_points);
        }
      }
      
      vw_out(InfoMessage) << " done.";
      vw_out(InfoMessage) << "     (" << interest_points.size() << " interest points found)\n";
      return interest_points;
    }

  };


  /// Get the orientation of the point at (i0,j0,k0).  This is done by
  /// computing a weighted histogram of edge orientations in a region
  /// around the detected point.  The weights for the weighted histogram
  /// are computed by multiplying the edge magnitude at each point by a
  /// gaussian weight.  The edge orientation histogram is then smoothed,
  /// effectively computing a kernel density estimate.  This density
  /// function is then searched for local peaks.
  template <class OriT, class MagT>
  int get_orientation( std::vector<float>& orientation,
                       ImageViewBase<OriT> const& ori,
                       ImageViewBase<MagT> const& mag,
                       int i0, int j0, float sigma_ratio = 1.0) {

    static const int FEATURE_ORI_NBINS =36;

    // NOTE: half width decreased from 20 for speed
    static const int IP_ORIENTATION_HALF_WIDTH = 5;

    // TODO: speed this step up!
    orientation.clear();
    // Nominal feature support patch is WxW at the base scale, with
    // W = IP_ORIENTATION_HALF_WIDTH * 2 + 1, and
    // we multiply by sigma[k]/sigma[1] for other planes.
  
    // Get bounds for scaled WxW window centered at (i,j) in plane k
    int halfwidth = (int)(IP_ORIENTATION_HALF_WIDTH*sigma_ratio + 0.5);
    int left  = i0 - halfwidth;
    int top   = j0 - halfwidth;
    int width = halfwidth*2+1;
    if ( (left >= 0) && (top >= 0) &&
         (left + width < (int)(ori.impl().cols())) &&
         (top + width < (int)(ori.impl().rows()))) {

      // Compute (gaussian weight)*(edge magnitude) kernel
      ImageView<float> weight(width,width);
      make_gaussian_kernel_2d( weight, 6 * sigma_ratio, width );
      // FIXME: Without the edge_extend here (and below) rasterization
      // will occasionally segfault. This concerns me.
      weight *= crop(edge_extend(mag.impl()),left,top,width,width);
    
      // Compute weighted histogram of edge orientations
      std::vector<float> histo;
      weighted_histogram( crop(edge_extend(ori.impl()),left,top,width,width),
                          weight, histo,
                          -M_PI, M_PI, FEATURE_ORI_NBINS );
    
      // Smooth histogram
      smooth_weighted_histogram( histo, 5.0 );

      // Find orientations at modes of the histogram
      std::vector<int> mode;
      find_weighted_histogram_mode( histo, mode );
      orientation.reserve(mode.size());
      for (unsigned m = 0; m < mode.size(); ++m)
        orientation.push_back( mode[m] * (2*M_PI/FEATURE_ORI_NBINS) - M_PI );
    }
  
    return 0;
  }

  /// This class performs interest point detection on a source image
  /// without using scale space methods.
  template <class InterestT>
  class InterestPointDetector : public InterestDetectorBase<InterestPointDetector<InterestT> > {
  public:

    /// Setting max_points = 0 will disable interest point culling.
    /// Otherwies, the max_points most "interesting" points are
    /// returned.
    InterestPointDetector(int max_points = 1000) : m_interest(InterestT()), m_max_points(max_points) {}

    /// Setting max_points = 0 will disable interest point culling.
    /// Otherwies, the max_points most "interesting" points are
    /// returned.
    InterestPointDetector(InterestT const& interest, int max_points = 1000) : m_interest(interest), m_max_points(max_points) {}

    /// Detect interest points in the source image.  
    template <class ViewT>
    InterestPointList process_image(ImageViewBase<ViewT> const& image) const {
      Timer total("\tTotal elapsed time", DebugMessage);

      // Calculate gradients, orientations and magnitudes
      vw_out(DebugMessage) << "\n\tCreating image data... ";
      Timer *timer = new Timer("done, elapsed time", DebugMessage);
      ImageInterestData<ViewT, InterestT> img_data(image);
      delete timer;

      // Compute interest image
      vw_out(DebugMessage) << "\tComputing interest image... ";
      {
        Timer t("done, elapsed time", DebugMessage);
        m_interest(img_data);
      }

      // Find extrema in interest image
      vw_out(DebugMessage) << "\tFinding extrema... ";
      InterestPointList points;
      {
        Timer t("elapsed time", DebugMessage);
        find_extrema(points, img_data);
        vw_out(DebugMessage) << "done (" << points.size() << " interest points), ";
      }

      // Subpixel localization
      vw_out(DebugMessage) << "\tLocalizing... ";
      {
        Timer t("elapsed time", DebugMessage);
        localize(points, img_data);
        vw_out(DebugMessage) << "done (" << points.size() << " interest points), ";
      }

      // Threshold (after localization)
      vw_out(DebugMessage) << "\tThresholding... ";
      {
        Timer t("elapsed time", DebugMessage);
        threshold(points, img_data);
        vw_out(DebugMessage) << "done (" << points.size() << " interest points), ";
      }
      
      // Cull (limit the number of interest points to the N "most interesting" points)
      vw_out(DebugMessage) << "\tCulling Interest Points (limit is set to " << m_max_points << " points)... ";
      {
        Timer t("elapsed time", DebugMessage);
        int original_num_points = points.size();
        points.sort();
        if ((m_max_points > 0) && (m_max_points < (int)points.size())) 
          points.resize(m_max_points);
        vw_out(DebugMessage) << "done (removed " << original_num_points - points.size() << " interest points, " << points.size() << " remaining.), ";
      }

      // Assign orientations
      vw_out(DebugMessage) << "\tAssigning orientations... ";
      {
        Timer t("elapsed time", DebugMessage);
        assign_orientations(points, img_data);
        vw_out(DebugMessage) << "done (" << points.size() << " interest points), ";
      }

      // Return vector of interest points
      return points;
    }

  protected:
    InterestT m_interest;
    int m_max_points;

    // By default, use find_peaks in Extrema.h
    template <class DataT>
    inline int find_extrema(InterestPointList& points, DataT const& img_data) const {
      return find_peaks(points, img_data);
    }

    // By default, use fit_peak in Localize.h
    template <class DataT>
    inline int localize(InterestPointList& points, DataT const& img_data) const {
      // TODO: Remove points rejected by localizer
      for (InterestPointList::iterator i = points.begin(); i != points.end(); ++i) {
        fit_peak(img_data.interest(), *i);
      }

      return 0;
    }

    template <class DataT>
    inline int threshold(InterestPointList& points, DataT const& img_data) const {
      // TODO: list::remove_if
      InterestPointList::iterator pos = points.begin();
      while (pos != points.end()) {
        if (!m_interest.threshold(*pos, img_data))
          pos = points.erase(pos);
        else
          pos++;
      }
    }

    template <class DataT>
    int assign_orientations(InterestPointList& points, DataT const& img_data) const {
      std::vector<float> orientation;
      // TODO: Need to do testing to figure out when it is more efficient to use
      // shallow views or fully rasterize up front. This may need to be decided at
      // runtime depending on the number of points and size of support region.
    
      // Ensure orientation and magnitude are fully rasterized.
      ImageView<typename DataT::pixel_type> ori = img_data.orientation();
      ImageView<typename DataT::pixel_type> mag = img_data.magnitude();

      for (InterestPointList::iterator i = points.begin(); i != points.end(); ++i) {
        get_orientation(orientation, ori, mag, (int)(i->x + 0.5), (int)(i->y + 0.5));
        if (!orientation.empty()) {
          i->orientation = orientation[0];
          for (int j = 1; j < orientation.size(); ++j) {
            InterestPoint pt = *i;
            pt.orientation = orientation[j];
            points.insert(i, pt);
          }
        }
      }
    }

    /// This method dumps the various images internal to the detector out
    /// to files for visualization and debugging.  The images written out
    /// are the x and y gradients, edge orientation and magnitude, and
    /// interest function values for the source image.
    template <class DataT>
    int write_images(DataT const& img_data) const {
      // Save the X gradient
      ImageView<float> grad_x_image = normalize(img_data.gradient_x());
      vw::write_image("grad_x.jpg", grad_x_image);

      // Save the Y gradient      
      ImageView<float> grad_y_image = normalize(img_data.gradient_y());
      vw::write_image("grad_y.jpg", grad_y_image);

      // Save the edge orientation image
      ImageView<float> ori_image = normalize(img_data.orientation());
      vw::write_image("ori.jpg", ori_image);

      // Save the edge magnitude image
      ImageView<float> mag_image = normalize(img_data.magnitude());
      vw::write_image("mag.jpg", mag_image);

      // Save the interest function image
      ImageView<float> interest_image = normalize(img_data.interest());
      vw::write_image("interest.jpg", interest_image);

      return 0;
    }
  };


  /// This class performs interest point detection on a source image
  /// making use of scale space methods to achieve scale invariance.
  /// This assumes that the detector works properly over different
  /// choices of scale.
  template <class InterestT>
  class ScaledInterestPointDetector : public InterestDetectorBase<ScaledInterestPointDetector<InterestT> > {
    
    ScaledInterestPointDetector(ScaledInterestPointDetector<InterestT> const& copy) {} 

  public:
    // TODO: choose number of octaves based on image size
    static const int IP_DEFAULT_SCALES = 3;
    static const int IP_DEFAULT_OCTAVES = 3;

    /// Setting max_points = 0 will disable interest point culling.
    /// Otherwies, the max_points most "interesting" points are
    /// returned.
    ScaledInterestPointDetector(int max_points = 1000)
      : m_interest(InterestT()), m_scales(IP_DEFAULT_OCTAVES), m_octaves(IP_DEFAULT_SCALES), m_max_points(max_points) {}

    /// Setting max_points = 0 will disable interest point culling.
    /// Otherwies, the max_points most "interesting" points are
    /// returned.
    ScaledInterestPointDetector(InterestT const& interest, int max_points = 1000) 
      : m_interest(interest), m_scales(IP_DEFAULT_OCTAVES), m_octaves(IP_DEFAULT_SCALES), m_max_points(max_points) {}

    /// Setting max_points = 0 will disable interest point culling.
    /// Otherwies, the max_points most "interesting" points are
    /// returned.
    ScaledInterestPointDetector(InterestT const& interest, int scales, int octaves, int max_points = 1000)
      : m_interest(interest), m_scales(scales), m_octaves(octaves), m_max_points(max_points) {}
    
    /// Detect interest points in the source image.
    template <class ViewT>
    InterestPointList process_image(ImageViewBase<ViewT> const& image) const {
      typedef ImageInterestData<ImageView<typename ViewT::pixel_type>,InterestT> DataT;

      Timer total("\t\tTotal elapsed time", DebugMessage);

      // Create scale space
      vw_out(DebugMessage) << "\n\tCreating initial image octave... ";
      Timer *t_oct = new Timer("done, elapsed time", DebugMessage);
      ImageOctave<typename DataT::source_type> octave(image, m_scales);
      delete t_oct;

      InterestPointList points;
      std::vector<DataT> img_data;

      for (int o = 0; o < m_octaves; ++o) {
        Timer t_loop("\t\tElapsed time for octave", DebugMessage);

        // Calculate intermediate data (gradients, orientations, magnitudes)
        vw_out(DebugMessage) << "\tCreating image data... ";
        {
          Timer t("done, elapsed time", DebugMessage);
          img_data.clear();
          img_data.reserve(octave.num_planes);
          for (int k = 0; k < octave.num_planes; ++k) {
            img_data.push_back(DataT(octave.scales[k]));
          }
        }

        // Compute interest images
        vw_out(DebugMessage) << "\tComputing interest images... ";
        {
          Timer t("done, elapsed time", DebugMessage);
          for (int k = 0; k < octave.num_planes; ++k) {
            m_interest(img_data[k], octave.plane_index_to_scale(k));
          }
        }
      
        // TODO: record history

        // Find extrema in interest image
        vw_out(DebugMessage) << "\tFinding extrema... ";
        InterestPointList new_points;
        {
          Timer t("elapsed time", DebugMessage);
          find_extrema(new_points, img_data, octave);
          vw_out(DebugMessage) << "done (" << new_points.size() << " extrema found), ";
        }

        // Subpixel localization
        vw_out(DebugMessage) << "\tLocalizing... ";
        {
          Timer t("elapsed time", DebugMessage);
          localize(new_points, img_data, octave);
          vw_out(DebugMessage) << "done (" << new_points.size() << " interest points), ";
        }

        // Threshold
        vw_out(DebugMessage) << "\tThresholding... ";
        {
          Timer t("elapsed time", DebugMessage);
          threshold(new_points, img_data, octave);
          vw_out(DebugMessage) << "done (" << new_points.size() << " interest points), ";
        }

        // Cull (limit the number of interest points to the N "most interesting" points)
        vw_out(DebugMessage) << "\tCulling Interest Points (limit is set to " << m_max_points << " points)... ";
        {
          Timer t("elapsed time", DebugMessage);
          int original_num_points = new_points.size();
          new_points.sort();
          if ((m_max_points > 0) && (m_max_points < (int)new_points.size())) 
            new_points.resize(m_max_points);
          vw_out(DebugMessage) << "done (removed " << original_num_points - new_points.size() << " interest points, " << new_points.size() << " remaining.), ";
        }
        
        // Assign orientations
        vw_out(DebugMessage) << "\tAssigning orientations... ";
        {
          Timer t("elapsed time", DebugMessage);
          assign_orientations(new_points, img_data, octave);
          vw_out(DebugMessage) << "done (" << new_points.size() << " interest points), ";
        }

        // Scale subpixel location to move back to original coords
        for (InterestPointList::iterator i = new_points.begin(); i != new_points.end(); ++i) {
          i->x *= octave.base_scale;
          i->y *= octave.base_scale;
          // TODO: make sure this doesn't screw up any post-processing
          i->ix = (int)( i->x + 0.5 );
          i->iy = (int)( i->y + 0.5 );
        }

        // Add newly found interest points
        points.splice(points.end(), new_points);
      
        // Build next octave of scale space
        if (o != m_octaves - 1) {
          vw_out(DebugMessage) << "\tBuilding next octave... ";
          Timer t("done, elapsed time", DebugMessage);
          octave.build_next();
        }
      }

      return points;
    }

  protected:
    InterestT m_interest;
    int m_scales, m_octaves, m_max_points;
    
    //ImageOctaveHistory<ImageInterestData<ImageViewRef<float>, InterestT> > *history;

    // By default, uses find_peaks in Extrema.h
    template <class DataT, class ViewT>
    inline int find_extrema(InterestPointList& points,
                            std::vector<DataT> const& img_data,
                            ImageOctave<ViewT> const& octave) const {
      return find_peaks(points, img_data, octave);
    }

    // By default, uses fit_peak in Localize.h
    template <class DataT, class ViewT>
    inline int localize(InterestPointList& points,
                        std::vector<DataT> const& img_data,
                        ImageOctave<ViewT> const& octave) const {
      for (InterestPointList::iterator i = points.begin(); i != points.end(); ++i) {
        fit_peak(img_data, *i, octave);
      }

      return 0;
    }

    template <class DataT, class ViewT>
    inline int threshold(InterestPointList& points,
                         std::vector<DataT> const& img_data,
                         ImageOctave<ViewT> const& octave) const {
      // TODO: list::remove_if
      InterestPointList::iterator pos = points.begin();
      while (pos != points.end()) {
        int k = octave.scale_to_plane_index(pos->scale);
        if (!m_interest.threshold(*pos, img_data[k]))
          pos = points.erase(pos);
        else
          pos++;
      }
    }

    template <class DataT, class ViewT>
    int assign_orientations(InterestPointList& points,
                            std::vector<DataT> const& img_data,
                            ImageOctave<ViewT> const& octave) const {
      std::vector<float> orientation;

      // TODO: shallow view vs. fully rasterized
      // Ensure orientation and magnitude are fully rasterized.
      // TODO: This is NOT memory efficient.
      /*
        std::vector<typename DataT::rasterize_type> ori(octave.num_planes);
        std::vector<typename DataT::rasterize_type> mag(octave.num_planes);
        for (int k = 0; k < octave.num_planes; ++k) {
        ori[k] = img_data[k].orientation();
        mag[k] = img_data[k].magnitude();
        }
      */

      for (InterestPointList::iterator i = points.begin(); i != points.end(); ++i) {
        int k = octave.scale_to_plane_index(i->scale);
        // NOTE: use i->ix, i->iy?
        get_orientation(orientation, img_data[k].orientation(), img_data[k].magnitude(),
                        (int)(i->x + 0.5), (int)(i->y + 0.5), octave.sigma[k]/octave.sigma[1]);
        if (!orientation.empty()) {
          i->orientation = orientation[0];
          for (int j = 1; j < orientation.size(); ++j) {
            InterestPoint pt = *i;
            pt.orientation = orientation[j];
            points.insert(i, pt);
          }
        }
      }
    }

    /// This method dumps the various images internal to the detector out
    /// to files for visualization and debugging.  The images written out
    /// are the x and y gradients, edge orientation and magnitude, and
    /// interest function values for all planes in the octave processed.
    template <class DataT>
    int write_images(std::vector<DataT> const& img_data) const {
      for (int k = 0; k < img_data.size(); ++k){
        int imagenum = k;
        char fname[256];

        // Save the scale
        sprintf( fname, "scale_%02d.jpg", imagenum );
        ImageView<float> scale_image = normalize(img_data[k].source());
        vw::write_image(fname, scale_image);
      
        // Save the X gradient
        sprintf( fname, "grad_x_%02d.jpg", imagenum );
        ImageView<float> grad_x_image = normalize(img_data[k].gradient_x());
        vw::write_image(fname, grad_x_image);

        // Save the Y gradient      
        sprintf( fname, "grad_y_%02d.jpg", imagenum );
        ImageView<float> grad_y_image = normalize(img_data[k].gradient_y());
        vw::write_image(fname, grad_y_image);

        // Save the edge orientation image
        sprintf( fname, "ori_%02d.jpg", imagenum );
        ImageView<float> ori_image = normalize(img_data[k].orientation());
        vw::write_image(fname, ori_image);

        // Save the edge magnitude image
        sprintf( fname, "mag_%02d.jpg", imagenum );
        ImageView<float> mag_image = normalize(img_data[k].magnitude());
        vw::write_image(fname, mag_image);

        // Save the interest function image
        sprintf( fname, "interest_%02d.jpg", imagenum );
        ImageView<float> interest_image = normalize(img_data[k].interest());
        vw::write_image(fname, interest_image);
      }
      return 0;
    }
  };

}} // namespace vw::ip 

#endif // __VW_INTERESTPOINT_DETECTOR_H__
