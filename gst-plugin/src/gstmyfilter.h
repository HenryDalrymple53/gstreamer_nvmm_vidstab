#ifndef __GST_MYFILTER_H__
#define __GST_MYFILTER_H__

#include <gst/gst.h>
#include <vpi/Stream.h>
#include <vpi/Image.h>
#include <vpi/Pyramid.h>
#include <vpi/Array.h>
#include <nvbufsurface.h>
#include <vpi/algo/OpticalFlowPyrLK.h>
#include <vpi/algo/GaussianPyramid.h>
#include <vpi/algo/PerspectiveWarp.h>
#include <vpi/algo/ConvertImageFormat.h>
#include <vpi/algo/HarrisCorners.h>
#include <vpi/algo/TransformEstimator.h>
G_BEGIN_DECLS

#define GST_TYPE_MYFILTER (gst_my_filter_get_type())
G_DECLARE_FINAL_TYPE (GstMyFilter, gst_my_filter, GST, MYFILTER, GstElement)

struct _GstMyFilter
{
  GstElement element;
  GstPad *sinkpad, *srcpad;
  gboolean silent;
  

  /* VPI stream */
  VPIStream vpi_stream;

  /* Frame dimensions and format - populated from caps */
  gint width;
  gint height;
  VPIImageFormat format;

  /* Optical flow pyramid state */
  VPIPyramid pyrPrevFrame;
  VPIPyramid pyrCurFrame;
  gint levels;
  gfloat scale;

  /* Keypoint tracking arrays */
  VPIArray arrPrevPts;
  VPIArray arrCurPts;
  VPIArray arrStatus;

  /* Score array for Harris Corner Detector*/
  VPIArray harrisScores;

  /* Create harris Payload */
  VPIPayload harris;
  VPIHarrisCornerDetectorParams harrisParams;
  uint8_t frameCount;

  /* Optical flow payload */
  VPIPayload optflow;
  VPIOpticalFlowPyrLKParams lkParams;

  /* Previous frame image for LK tracking */
  VPIImage prevImage;
  VPIImage yImage;

  /*Gaussian pyramid image used in Harris Edge Detection*/
  VPIImage harrisGauss;

  /* Flag to know if VPI objects have been initialized post-caps */
  gboolean vpi_initialized;

  /*warped imgae*/
  VPIImage warpedImage;

  NvBufSurface *warpedSurface;


  /*TransformEstimator*/
  VPITransformEstimatorParams transformParams;
  VPIPayload transformPayload;
  VPIArray transform;
  VPIArray arrMatches;

  NvBufSurface *imageSurface;
  VPIImage curImage;



};

G_END_DECLS
#endif /* __GST_MYFILTER_H__ */
