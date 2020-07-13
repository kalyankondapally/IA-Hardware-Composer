sudo EGL_LOG_LEVEL=debug MESA_DEBUG=1 LD_LIBRARY_PATH=/home/kalyan/install-kms/lib/:/home/kalyan/install-kms/lib/x86_64-linux-gnu ./linux_test -f 50 -j ./jsonconfigs/kmscube1layer.json  >& log.txt

#video1layer_nv12.json
# By default we use hybrid mode for all layers if more than 1 intel gpu are available on the system. Note that individual layer settings can over ride if that layer want's to be
# rendered on a particular device. This setting should be added to json file.
# Application Wide Settings:
# -d 1 -> Will force disable hybrid mode for all layers.
# -m 1 -> Will enable hybrid mode only for Media layers
# -r 1 -> Will enable hybrid mode only for 3d layers

# Layer settings which should be added in json file under "layers_Parameters":
# "prefer_device" : 0 for layer to use scanout device, 1 for layer to use render device, 2 for layer to use media device,
