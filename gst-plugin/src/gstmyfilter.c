#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include "gstmyfilter.h"
#include <math.h>


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

  filter->silent           = FALSE;
  filter->vpi_initialized  = FALSE;
  filter->width            = 0;
  filter->height           = 0;
  filter->vpi_stream       = NULL;
  filter->pyrPrevFrame     = NULL;
  filter->pyrCurFrame      = NULL;
  filter->arrPrevPts       = NULL;
  filter->arrCurPts        = NULL;
  filter->arrStatus        = NULL;
  filter->optflow          = NULL;
  filter->prevImage        = NULL;
  filter->harrisScores     = NULL;
  filter->harrisGauss      = NULL;
  filter->harris           = NULL;
  filter->frameCount       = 0;
  filter->warpedImage      = NULL;
  filter->yImage           = NULL;
  filter->warpedSurface    = NULL;
  filter->refreshHarris = true;
  filter->cumX = 0.0;
  filter->cumY = 0.0;


  vpiStreamCreate(VPI_BACKEND_CUDA | VPI_BACKEND_CPU, &filter->vpi_stream);
}

/* Tears down all VPI/NvBuf objects, leaving pointers NULL. */
static void
gst_my_filter_teardown_vpi (GstMyFilter * filter)
{
  /* Payloads */
  if (filter->optflow)          { vpiPayloadDestroy(filter->optflow);          filter->optflow          = NULL; }
  if (filter->harris)           { vpiPayloadDestroy(filter->harris);           filter->harris           = NULL; }

  /* Pyramids */
  if (filter->pyrPrevFrame) { vpiPyramidDestroy(filter->pyrPrevFrame); filter->pyrPrevFrame = NULL; }
  if (filter->pyrCurFrame)  { vpiPyramidDestroy(filter->pyrCurFrame);  filter->pyrCurFrame  = NULL; }

  /* Arrays */
  if (filter->arrPrevPts)   { vpiArrayDestroy(filter->arrPrevPts);   filter->arrPrevPts   = NULL; }
  if (filter->arrCurPts)    { vpiArrayDestroy(filter->arrCurPts);    filter->arrCurPts    = NULL; }
  if (filter->arrStatus)    { vpiArrayDestroy(filter->arrStatus);    filter->arrStatus    = NULL; }
  if (filter->harrisScores) { vpiArrayDestroy(filter->harrisScores); filter->harrisScores = NULL; }

  /* Images — harrisGauss is a wrapper (pyramid level), destroy before the pyramid */
  if (filter->harrisGauss)  { vpiImageDestroy(filter->harrisGauss);  filter->harrisGauss  = NULL; }
  if (filter->prevImage)    { vpiImageDestroy(filter->prevImage);     filter->prevImage    = NULL; }
  if (filter->yImage)       { vpiImageDestroy(filter->yImage);        filter->yImage       = NULL; }
  /* warpedImage is a wrapper over warpedSurface — destroy wrapper before surface */
  if (filter->warpedImage)  { vpiImageDestroy(filter->warpedImage);   filter->warpedImage  = NULL; }

  /* NvBufSurface */
  if (filter->warpedSurface) { NvBufSurfaceDestroy(filter->warpedSurface); filter->warpedSurface = NULL; }

  filter->vpi_initialized = FALSE;
}

static void
gst_my_filter_finalize (GObject * object)
{
  GstMyFilter *filter = GST_MYFILTER (object);

  gst_my_filter_teardown_vpi (filter);

  if (filter->vpi_stream) { vpiStreamDestroy(filter->vpi_stream); filter->vpi_stream = NULL; }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}


static gboolean
gst_my_filter_setup_vpi (GstMyFilter * filter)
{
  filter->levels = 4;
  filter->scale  = 0.5f;
  filter->format = VPI_IMAGE_FORMAT_U8;

  /* Grayscale working image */
  vpiImageCreate(filter->width, filter->height,
      VPI_IMAGE_FORMAT_U8, VPI_BACKEND_CUDA, &filter->yImage);

  /* Pyramids */
  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrPrevFrame);
  vpiPyramidCreate(filter->width, filter->height,
      filter->format, filter->levels, filter->scale, 0,
      &filter->pyrCurFrame);

  /* Arrays */
  #define MAX_KEYPOINTS 1000
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0, &filter->arrPrevPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_KEYPOINT_F32, 0, &filter->arrCurPts);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_U8,           0, &filter->arrStatus);
  vpiArrayCreate(MAX_KEYPOINTS, VPI_ARRAY_TYPE_U32,          0, &filter->harrisScores);

  /* LK optical flow */
  vpiInitOpticalFlowPyrLKParams(VPI_BACKEND_CUDA, &filter->lkParams);
  vpiCreateOpticalFlowPyrLK(VPI_BACKEND_CUDA,
                            filter->width, filter->height,
                            filter->format,
                            filter->levels, filter->scale,
                            &filter->optflow);

  /* Harris corner detector */
  vpiInitHarrisCornerDetectorParams(&filter->harrisParams);
  filter->harrisParams.gradientSize    = 7;     /* largest — captures more gradient info */
filter->harrisParams.blockSize       = 7;     /* largest — more robust corner response */
filter->harrisParams.strengthThresh  = 5.0f;  /* no threshold — keep all corners */
filter->harrisParams.sensitivity     = 0.01f; /* very low — keeps weak corners too */
filter->harrisParams.minNMSDistance  = 1;     /* tight — allows dense point clusters */
  vpiCreateHarrisCornerDetector(VPI_BACKEND_CUDA,
                                filter->width, filter->height,
                                &filter->harris);



  
  /* Warped output surface + VPI wrapper */
  NvBufSurfaceCreateParams params = {0};
  params.gpuId       = 0;
  params.width       = filter->width;
  params.height      = filter->height;
  params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  params.memType     = NVBUF_MEM_DEFAULT;
  params.layout      = NVBUF_LAYOUT_PITCH;
  NvBufSurfaceCreate(&filter->warpedSurface, 1, &params);

  VPIImageData warped_data;
  memset(&warped_data, 0, sizeof(warped_data));
  warped_data.bufferType = VPI_IMAGE_BUFFER_NVBUFFER;
  warped_data.buffer.fd  = filter->warpedSurface->surfaceList[0].bufferDesc;
  vpiImageCreateWrapper(&warped_data, NULL, VPI_BACKEND_CUDA, &filter->warpedImage);

  /* harrisGauss is created per-frame as a pyramid-level wrapper — not here */

  GST_DEBUG_OBJECT(filter, "VPI setup complete: optflow=%p", (void *)filter->optflow);
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

      if (filter->vpi_initialized)
        gst_my_filter_teardown_vpi (filter);

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

static void
mat3_multiply (VPIPerspectiveTransform result,
               const VPIPerspectiveTransform a,
               const VPIPerspectiveTransform b)
{
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      result[i][j] = 0.0f;
      for (int k = 0; k < 3; k++)
        result[i][j] += a[i][k] * b[k][j];
    }
}

static float
fmin_f (float x, float y) { return x < y ? x : y; }


static GstFlowReturn
gst_my_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstMyFilter *filter = GST_MYFILTER (parent);

  if (!filter->vpi_initialized) {
    GST_WARNING_OBJECT (filter, "VPI not initialised, passing buffer through");
    return gst_pad_push (filter->srcpad, buf);
  }

  /* ── 1. Map GStreamer buffer → NvBufSurface ── */
  GstMapInfo map_info;
  if (!gst_buffer_map(buf, &map_info, GST_MAP_READ)) {
    GST_ERROR_OBJECT(filter, "Failed to map input buffer");
    return GST_FLOW_ERROR;
  }
  NvBufSurface *surface = (NvBufSurface *)map_info.data;

  /* ── 2. Wrap current frame as VPIImage (zero-copy dmabuf) ── */
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

  /* ── 3+4. Submit RGBA→Y8 and pyramid back-to-back, no sync between them ── */
  status = vpiSubmitConvertImageFormat(filter->vpi_stream, VPI_BACKEND_CUDA,
                                       curImage, filter->yImage, NULL);
  if (status != VPI_SUCCESS) {
    GST_ERROR_OBJECT(filter, "ConvertImageFormat failed: %s", vpiStatusGetName(status));
    goto error_destroy_cur;
  }

  status = vpiSubmitGaussianPyramidGenerator(filter->vpi_stream, VPI_BACKEND_CUDA,
                                             filter->yImage, filter->pyrCurFrame,
                                             VPI_BORDER_CLAMP);
  if (status != VPI_SUCCESS) {
    GST_ERROR_OBJECT(filter, "GaussianPyramid failed: %s", vpiStatusGetName(status));
    goto error_destroy_cur;
  }

  /* ── 5. Harris refresh every 30 frames ──
   * Harris writes arrPrevPts which LK reads, so we must sync before Harris
   * and again after before LK. This is the only unavoidable double-sync. ── */
  if (filter->refreshHarris) {
    /* Sync so pyramid is complete before Harris reads it */
    vpiStreamSync(filter->vpi_stream);

    /* Recreate harrisGauss wrapper pointing at the current pyramid level */
    if (filter->harrisGauss) { vpiImageDestroy(filter->harrisGauss); filter->harrisGauss = NULL; }
    vpiImageCreateWrapperPyramidLevel(filter->pyrCurFrame, 0, &filter->harrisGauss);

    status = vpiSubmitHarrisCornerDetector(filter->vpi_stream, VPI_BACKEND_CUDA,
                                           filter->harris, filter->harrisGauss,
                                           filter->arrPrevPts, filter->harrisScores,
                                           &filter->harrisParams);
    if (status != VPI_SUCCESS) {
      GST_ERROR_OBJECT(filter, "Harris failed: %s", vpiStatusGetName(status));
      goto error_destroy_cur;

    VPIArray   t = filter->arrPrevPts;   
    filter->arrPrevPts   = filter->arrCurPts;   
    filter->arrCurPts   = t;

    }

    /* Sync so arrPrevPts is populated before LK uses it */
    vpiStreamSync(filter->vpi_stream);

    VPIArrayData dbg;
    vpiArrayLockData(filter->arrPrevPts, VPI_LOCK_READ, VPI_ARRAY_BUFFER_HOST_AOS, &dbg);
    GST_DEBUG_OBJECT(filter, "Harris detected %d keypoints", *dbg.buffer.aos.sizePointer);
    vpiArrayUnlock(filter->arrPrevPts);
    filter->refreshHarris = false;
  }
  filter->frameCount++;

  /* ── 6-7. Optical flow + warp (only once we have a previous frame) ── */
  if (filter->prevImage != NULL) {

    /* Submit LK — GPU starts tracking while CPU continues below */
    status = vpiSubmitOpticalFlowPyrLK(filter->vpi_stream, VPI_BACKEND_CUDA,
                                       filter->optflow,
                                       filter->pyrPrevFrame, filter->pyrCurFrame,
                                       filter->arrPrevPts,   filter->arrCurPts,
                                       filter->arrStatus,
                                       &filter->lkParams);
    if (status != VPI_SUCCESS) {
      GST_ERROR_OBJECT(filter, "OpticalFlowPyrLK failed: %s", vpiStatusGetName(status));
      goto error_destroy_cur;
    }
    vpiStreamSync(filter->vpi_stream);
    VPIArrayData prevPts;
    VPIArrayData curPts;
    VPIArrayData status;

    vpiArrayLockData(filter->arrPrevPts, VPI_LOCK_READ, VPI_ARRAY_BUFFER_HOST_AOS,&prevPts);
    vpiArrayLockData(filter->arrCurPts, VPI_LOCK_READ, VPI_ARRAY_BUFFER_HOST_AOS,&curPts);
    vpiArrayLockData(filter->arrStatus, VPI_LOCK_READ, VPI_ARRAY_BUFFER_HOST_AOS,&status);

    VPIKeypointF32 *prevPtsArr = (VPIKeypointF32 *)prevPts.buffer.aos.data;
    VPIKeypointF32 *curPtsArr = (VPIKeypointF32 *)curPts.buffer.aos.data;
    uint8_t *statusArr = (uint8_t *)status.buffer.aos.data;
    int n = *curPts.buffer.aos.sizePointer;
    int numGoodPts = 0;
    float xDif = 0.0f;
    float yDif = 0.0f;

    for(int i = 0; i < n; i++){
      if(statusArr[i]==0){
        xDif += (curPtsArr[i].x - prevPtsArr[i].x);
        yDif += (curPtsArr[i].y - prevPtsArr[i].y);
        numGoodPts++;
      }
      
    }

    if (numGoodPts > 0) {
        xDif /= numGoodPts;
        yDif /= numGoodPts;
    } else {
        xDif = 0.0f;
        yDif = 0.0f;
    }
    filter->cumX = filter->cumX * 0.95f + xDif;
    filter->cumY = filter->cumY * 0.95f + yDif;

    vpiArrayUnlock(filter->arrPrevPts);
    vpiArrayUnlock(filter->arrCurPts);
    vpiArrayUnlock(filter->arrStatus);


    /* ── CPU work while GPU runs LK ──
     * Build the warp matrix now so it's ready the moment we need it.
     * This runs in parallel with the GPU optical flow above. ── */
    double rad         = filter->frameCount * 3 * M_PI / 180.0;
    double w           = (double)filter->width;
    double h           = (double)filter->height;
    double scaleX      = 1.0 / (fabs(cos(rad)) + fabs(sin(rad)) * h / w);
    double scaleY      = 1.0 / (fabs(cos(rad)) + fabs(sin(rad)) * w / h);
    double aspectScale = fmin_f((float)scaleX, (float)scaleY);
    //GST_DEBUG_OBJECT(filter, "xDif: %f \t yDif: %f", -filter->cumX, -filter->cumY);


    VPIPerspectiveTransform rotation    = { { 1.0, 0.0, -filter->cumX },
                                            { 0.0,  1.0, -filter->cumY },
                                            { 0.0,       0.0,      1.0 } };
    VPIPerspectiveTransform to_center   = { { 1.0, 0.0,  filter->width  / 2.0f },
                                            { 0.0, 1.0,  filter->height / 2.0f },
                                            { 0.0, 0.0,  1.0 } };
    VPIPerspectiveTransform from_center = { { 1.0, 0.0, -filter->width  / 2.0f },
                                            { 0.0, 1.0, -filter->height / 2.0f },
                                            { 0.0, 0.0,  1.0 } };
    VPIPerspectiveTransform scale_mat   = { { 1.0, 0.0,         0.0 },
                                            { 0.0,         1.0, 0.0 },
                                            { 0.0,         0.0,         1.0 } };
    VPIPerspectiveTransform tmp, translate, final_mat;
    mat3_multiply(tmp,       to_center,  scale_mat);
    mat3_multiply(translate, tmp,        rotation);
    mat3_multiply(final_mat, translate,  from_center);

    /* ── Single sync: wait for LK to finish ── */
    vpiStreamSync(filter->vpi_stream);

   



    /* Submit warp immediately after — GPU can pipeline this behind the CPU transform ── */
    vpiSubmitPerspectiveWarp(filter->vpi_stream, VPI_BACKEND_CUDA,
                                      curImage, final_mat, filter->warpedImage,
                                      NULL, VPI_INTERP_LINEAR, VPI_BORDER_ZERO, 0);
        

    /* Final sync — waits for both transform estimator and warp to finish ── */
    vpiStreamSync(filter->vpi_stream);



    NvBufSurfaceCopy(filter->warpedSurface, surface);
  }

  /* ── 8. Advance state: update prevImage, swap pyramids and point arrays ── */
  if (filter->prevImage) { vpiImageDestroy(filter->prevImage); filter->prevImage = NULL; }
  filter->prevImage = curImage;  /* takes ownership */
  curImage = NULL;

  { VPIPyramid t = filter->pyrPrevFrame; filter->pyrPrevFrame = filter->pyrCurFrame; filter->pyrCurFrame = t; }

  gst_buffer_unmap(buf, &map_info);
  return gst_pad_push(filter->srcpad, buf);

error_destroy_cur:
  if (curImage) vpiImageDestroy(curImage);
  gst_buffer_unmap(buf, &map_info);
  return GST_FLOW_ERROR;
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