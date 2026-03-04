#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include "gstmyfilter.h"

GST_DEBUG_CATEGORY_STATIC (gst_my_filter_debug);
#define GST_CAT_DEFAULT gst_my_filter_debug

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
        "format = (string) NV12, "
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
        "format = (string) NV12, "
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

/* ── class init ──────────────────────────────────────────────────────────── */

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

/* ── instance init ───────────────────────────────────────────────────────── */

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

  vpiStreamCreate(0, &filter->vpi_stream);
}

/* ── finalize ────────────────────────────────────────────────────────────── */

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

/* ── VPI setup (called once caps are known) ──────────────────────────────── */

static gboolean
gst_my_filter_setup_vpi (GstMyFilter * filter)
{
  /* Pyramid parameters - adjust levels/scale to suit your motion range */
  filter->levels = 4;
  filter->scale  = 0.5f;

  /* NV12 maps to VPI_IMAGE_FORMAT_NV12 */
  filter->format = VPI_IMAGE_FORMAT_NV12_ER;

  /* Pyramids for previous and current frame */
  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrPrevFrame);

  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrCurFrame);

  /* Keypoint arrays - 1000 points is a reasonable starting budget */
#define MAX_KEYPOINTS 1000
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0,
      &filter->arrPrevPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0,
      &filter->arrCurPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_U8, 0,
      &filter->arrStatus);

  /* LK params and payload */
  vpiInitOpticalFlowPyrLKParams(VPI_BACKEND_CUDA, &filter->lkParams);

  vpiCreateOpticalFlowPyrLK(VPI_BACKEND_CUDA,
      filter->width, filter->height,
      filter->format,
      filter->levels, filter->scale,
      &filter->optflow);

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
        vpiPayloadDestroy(filter->optflow);      filter->optflow     = NULL;
        vpiPyramidDestroy(filter->pyrPrevFrame); filter->pyrPrevFrame = NULL;
        vpiPyramidDestroy(filter->pyrCurFrame);  filter->pyrCurFrame  = NULL;
        vpiArrayDestroy(filter->arrPrevPts);     filter->arrPrevPts   = NULL;
        vpiArrayDestroy(filter->arrCurPts);      filter->arrCurPts    = NULL;
        vpiArrayDestroy(filter->arrStatus);      filter->arrStatus    = NULL;
        if (filter->prevImage) {
          vpiImageDestroy(filter->prevImage);    filter->prevImage    = NULL;
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
  GstMapInfo map_info;
  gst_buffer_map(buf, &map_info, GST_MAP_READ);
  NvBufSurface *surface = (NvBufSurface *)map_info.data;

  /* TODO:
   * 1. Wrap buf into VPIImage (zero copy via NVMM)
   * 2. Build pyramid for current frame
   * 3. Submit LK optical flow (prev → cur)
   * 4. vpiStreamSync()
   * 5. CPU: fit homography + smooth
   * 6. Submit perspective warp on CUDA
   * 7. vpiStreamSync()
   * 8. Unwrap VPIImage, push buffer
   */

   // 1


  return gst_pad_push (filter->srcpad, buf);
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