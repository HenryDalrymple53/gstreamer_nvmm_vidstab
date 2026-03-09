#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include "gstmyfilter.h"

GST_DEBUG_CATEGORY_STATIC (gst_my_filter_debug);
#define GST_CAT_DEFAULT gst_my_filter_debug
#define MAX_FRAMES_HARRIS 30

enum
{
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE (
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        "video/x-raw(memory:NVMM), "
        "format = (string) RGBA, "
        "width = (int) [ 1, 32767 ], "
        "height = (int) [ 1, 32767 ], "
        "framerate = (fraction) [ 0/1, 2147483647/1 ]"
    )
);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        "video/x-raw(memory:NVMM), "
        "format = (string) RGBA, "
        "width = (int) [ 1, 32767 ], "
        "height = (int) [ 1, 32767 ], "
        "framerate = (fraction) [ 0/1, 2147483647/1 ]"
    )
);

#define gst_my_filter_parent_class parent_class
G_DEFINE_TYPE (GstMyFilter, gst_my_filter, GST_TYPE_ELEMENT);

GST_ELEMENT_REGISTER_DEFINE (my_filter, "myfilter", GST_RANK_NONE,
    GST_TYPE_MYFILTER);

static void gst_my_filter_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_my_filter_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_my_filter_finalize (GObject * object);
static gboolean gst_my_filter_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstFlowReturn gst_my_filter_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);


static void
gst_my_filter_class_init (GstMyFilterClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_my_filter_set_property;
  gobject_class->get_property = gst_my_filter_get_property;
  gobject_class->finalize     = gst_my_filter_finalize;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  gst_element_class_set_details_simple (gstelement_class,
      "MyFilter", "Video/Filter",
      "Low-latency VPI video stabilisation", "henry <<user@hostname.org>>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));
}

static void
gst_my_filter_init (GstMyFilter * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,
      GST_DEBUG_FUNCPTR (gst_my_filter_sink_event));
  gst_pad_set_chain_function (filter->sinkpad,
      GST_DEBUG_FUNCPTR (gst_my_filter_chain));
  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  GST_PAD_SET_PROXY_CAPS (filter->srcpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->srcpad);

  filter->silent          = FALSE;
  filter->vpi_initialized = FALSE;
  filter->width           = 0;
  filter->height          = 0;
  filter->vpi_stream      = NULL;
  filter->pyrPrevFrame    = NULL;
  filter->pyrCurFrame     = NULL;
  filter->arrPrevPts      = NULL;
  filter->arrCurPts       = NULL;
  filter->arrStatus       = NULL;
  filter->optflow         = NULL;
  filter->prevImage       = NULL;
  filter->harrisScores          = NULL;
  filter->harrisGauss     = NULL;
  filter->harris          = NULL;
  filter->frameCount    = 0;

  vpiStreamCreate(0, &filter->vpi_stream);
}

static void
gst_my_filter_finalize (GObject * object)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  if (filter->optflow)      vpiPayloadDestroy(filter->optflow);
  if (filter->pyrPrevFrame) vpiPyramidDestroy(filter->pyrPrevFrame);
  if (filter->pyrCurFrame)  vpiPyramidDestroy(filter->pyrCurFrame);
  if (filter->arrPrevPts)   vpiArrayDestroy(filter->arrPrevPts);
  if (filter->arrCurPts)    vpiArrayDestroy(filter->arrCurPts);
  if (filter->arrStatus)    vpiArrayDestroy(filter->arrStatus);
  if (filter->prevImage)    vpiImageDestroy(filter->prevImage);
  if (filter->vpi_stream)   vpiStreamDestroy(filter->vpi_stream);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static gboolean
gst_my_filter_setup_vpi (GstMyFilter * filter)
{
  /* Pyramid parameters */
  filter->levels = 4;
  filter->scale  = 0.5f;

  filter->format = VPI_IMAGE_FORMAT_U8;
  vpiImageCreate(filter->width, filter->height,
      VPI_IMAGE_FORMAT_U8, VPI_BACKEND_CUDA, &filter->yImage);

  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrPrevFrame);

  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrCurFrame);

  #define MAX_KEYPOINTS 1000
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0,
      &filter->arrPrevPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0,
      &filter->arrCurPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_U8, 0,
      &filter->arrStatus);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_U32, 0, 
      &filter->harrisScores);

  /* LK params and payload */
  vpiInitOpticalFlowPyrLKParams(VPI_BACKEND_CUDA, &filter->lkParams);
  vpiInitHarrisCornerDetectorParams(&filter->harrisParams);
  filter->harrisParams.sensitivity = 0.01;

  vpiCreateOpticalFlowPyrLK(VPI_BACKEND_CUDA,
      filter->width, filter->height,
      filter->format,
      filter->levels, filter->scale,
      &filter->optflow);

  vpiCreateHarrisCornerDetector(VPI_BACKEND_CUDA, 
    filter->width, 
    filter->height,
    &filter->harris);

 
GST_DEBUG("  optflow (payload): %p", (void *)(filter->optflow));
  filter->vpi_initialized = TRUE;
  return TRUE;
}

static void
gst_my_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);
  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_my_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstMyFilter *filter = GST_MYFILTER (object);
  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_my_filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstMyFilter *filter = GST_MYFILTER (parent);
  gboolean ret;

  GST_LOG_OBJECT (filter, "Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps      *caps;
      GstStructure *s;

      gst_event_parse_caps (event, &caps);
      s = gst_caps_get_structure (caps, 0);

      gst_structure_get_int (s, "width",  &filter->width);
      gst_structure_get_int (s, "height", &filter->height);

      GST_DEBUG_OBJECT (filter, "Caps: %dx%d", filter->width, filter->height);

      /* Tear down any previous VPI objects if caps changed mid-stream */
      if (filter->vpi_initialized) {
        vpiPayloadDestroy(filter->optflow);      filter->optflow      = NULL;
        vpiPyramidDestroy(filter->pyrPrevFrame); filter->pyrPrevFrame = NULL;
        vpiPyramidDestroy(filter->pyrCurFrame);  filter->pyrCurFrame  = NULL;
        vpiArrayDestroy(filter->arrPrevPts);     filter->arrPrevPts   = NULL;
        vpiArrayDestroy(filter->arrCurPts);      filter->arrCurPts    = NULL;
        vpiArrayDestroy(filter->arrStatus);      filter->arrStatus    = NULL;
        if (filter->prevImage) {
          vpiImageDestroy(filter->prevImage);    filter->prevImage    = NULL;
        }
        if (filter->yImage) {
          vpiImageDestroy(filter->yImage);       filter->yImage       = NULL;
        }
        filter->vpi_initialized = FALSE;
      }

      gst_my_filter_setup_vpi (filter);

      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }
  return ret;
}

static GstFlowReturn
gst_my_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstMyFilter *filter = GST_MYFILTER (parent);

  if (!filter->vpi_initialized) {
    GST_WARNING_OBJECT (filter, "VPI not initialised, passing buffer through");
    return gst_pad_push (filter->srcpad, buf);
  }

  // 1. Wrap NvBufSurface (RGBA NVMM) into VPIImage (zero-copy via fd)
  GstMapInfo map_info;
  if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
    GST_ERROR_OBJECT(filter, "Failed to map input buffer");
    return GST_FLOW_ERROR;
  }

  NvBufSurface *surface = (NvBufSurface *)map_info.data;
 

  // Zero-copy wrap of RGBA NVMM buffer via dmabuf fd
  VPIImageData img_data;
  memset(&img_data, 0, sizeof(img_data));
  img_data.bufferType = VPI_IMAGE_BUFFER_NVBUFFER;
  img_data.buffer.fd  = surface->surfaceList[0].bufferDesc;

  VPIImage curImage = NULL;
  VPIStatus status = vpiImageCreateWrapper(&img_data, NULL, VPI_BACKEND_CUDA, &curImage);
  if (status != VPI_SUCCESS) {
    GST_ERROR_OBJECT(filter, "vpiImageCreateWrapper failed: %s", vpiStatusGetName(status));
    gst_buffer_unmap(buf, &map_info);
    return GST_FLOW_ERROR;
  }


  status = vpiSubmitConvertImageFormat(filter->vpi_stream, VPI_BACKEND_CUDA,
      curImage, filter->yImage, NULL);
  if (status != VPI_SUCCESS) {
    GST_ERROR_OBJECT(filter, "ConvertImageFormat RGBA->Y8 failed: %s", vpiStatusGetName(status));
    vpiImageDestroy(curImage);
    gst_buffer_unmap(buf, &map_info);
    return GST_FLOW_ERROR;
  }

  // 3. Build Gaussian pyramid for current frame (on U16)
  status = vpiSubmitGaussianPyramidGenerator(filter->vpi_stream,
                                             VPI_BACKEND_CUDA,
                                             filter->yImage,
                                             filter->pyrCurFrame,
                                             VPI_BORDER_CLAMP);
  if (status != VPI_SUCCESS) {
    GST_ERROR_OBJECT(filter, "GaussianPyramid failed: %s", vpiStatusGetName(status));
    vpiImageDestroy(curImage);
    gst_buffer_unmap(buf, &map_info);
    return GST_FLOW_ERROR;
  }

  vpiImageCreateWrapperPyramidLevel(filter->pyrCurFrame, 0, &filter->harrisGauss);

  


  //Logic for Harris Edge detection using VPI
  status = vpiSubmitHarrisCornerDetector(filter->vpi_stream,
                                VPI_BACKEND_CUDA,
                                filter->harris,
                                filter->harrisGauss,
                                filter->arrCurPts,
                                filter->harrisScores,
                                &filter->harrisParams);
  if (status != VPI_SUCCESS) {
      GST_ERROR_OBJECT(filter, "harris corner detector failed: %s", vpiStatusGetName(status));
      vpiImageDestroy(curImage);
      gst_buffer_unmap(buf, &map_info);
      return GST_FLOW_ERROR;
  } 


  // 4. Only run optical flow if we have a previous frame
  if (filter->prevImage != NULL) {
    status = vpiSubmitOpticalFlowPyrLK(filter->vpi_stream,
                                       VPI_BACKEND_CUDA,
                                       filter->optflow,
                                       filter->pyrPrevFrame,
                                       filter->pyrCurFrame,
                                       filter->arrPrevPts,
                                       filter->arrCurPts,
                                       filter->arrStatus,
                                       &filter->lkParams);
    if (status != VPI_SUCCESS) {
      GST_ERROR_OBJECT(filter, "OpticalFlowPyrLK failed: %s", vpiStatusGetName(status));
      vpiImageDestroy(curImage);
      gst_buffer_unmap(buf, &map_info);
      return GST_FLOW_ERROR;
    } 

  }

  // 5. Wait for GPU work to finish
  vpiStreamSync(filter->vpi_stream);

  // TODO 6: CPU - read arrCurPts/arrPrevPts, fit homography, smooth it

  // TODO 7: vpiSubmitPerspectiveWarp() on CUDA

  // TODO 8: vpiStreamSync() after warp

  // 9. Swap prev <-> cur for next frame
  if (filter->prevImage != NULL) {
    vpiImageDestroy(filter->prevImage);
  }
  filter->prevImage = curImage;
  curImage = NULL;

  VPIPyramid tmpPyr    = filter->pyrPrevFrame;
  filter->pyrPrevFrame = filter->pyrCurFrame;
  filter->pyrCurFrame  = tmpPyr;

  VPIArray tmpArr    = filter->arrPrevPts;
  filter->arrPrevPts = filter->arrCurPts;
  filter->arrCurPts  = tmpArr;

  gst_buffer_unmap(buf, &map_info);
  return gst_pad_push(filter->srcpad, buf);
}

static gboolean
myfilter_init (GstPlugin * myfilter)
{
  GST_DEBUG_CATEGORY_INIT (gst_my_filter_debug, "myfilter",
      0, "VPI video stabilisation filter");
  return GST_ELEMENT_REGISTER (my_filter, myfilter);
}

#ifndef PACKAGE
#define PACKAGE "myfirstmyfilter"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    myfilter, "myfilter", myfilter_init,
    PACKAGE_VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)