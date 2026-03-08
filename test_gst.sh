GST_DEBUG=myfilter:5 gst-launch-1.0 \
  videotestsrc ! \
  'video/x-raw,format=I420' ! \
  nvvidconv ! \
  'video/x-raw(memory:NVMM),format=RGBA' ! \
  nvvidconv bl-output=false ! \
  'video/x-raw(memory:NVMM),format=RGBA' ! \
  myfilter ! \
  nvvidconv ! \
  'video/x-raw,format=I420' ! \
  jpegenc ! rtpjpegpay ! udpsink host=192.168.1.100 port=5000
wait

