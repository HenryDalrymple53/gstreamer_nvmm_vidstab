GST_DEBUG=myfilter:5 gst-launch-1.0 \
  v4l2src device=/dev/rover/camera_infrared ! \
  'image/jpeg,width=1920,height=1080,framerate=30/1' ! \
  jpegdec ! \
  nvvidconv ! \
  'video/x-raw(memory:NVMM),format=RGBA' ! \
  myfilter ! \
  nvvidconv ! \
  'video/x-raw,format=I420' ! \
  nvjpegenc ! \
  rtpjpegpay ! \
  udpsink host=192.168.1.100 port=5000