#!/bin/sh
# Install the grate Mesa build over the system one. ninja install only writes
# to the meson prefix (~/Dev/stage-main); without this the copy in /usr/lib
# goes stale and everything not using LD_LIBRARY_PATH tests old code.
set -eu
S=/home/sam/Dev/stage-main
ninja -C /home/sam/Dev/mesa/build-grate install >/dev/null
for f in libEGL.so.1.0.0 libGLESv2.so.2.0.0 libGLESv1_CM.so.1.1.0 libGL.so.1.2.0 \
         libgbm.so.1.0.0 libgallium-26.3.0-devel.so; do
    echo samuca | sudo -S cp -f "$S/lib/$f" "/usr/lib/$f" 2>/dev/null
done
echo samuca | sudo -S mkdir -p /usr/lib/gbm 2>/dev/null
echo samuca | sudo -S cp -f "$S/lib/gbm/dri_gbm.so" /usr/lib/gbm/dri_gbm.so 2>/dev/null
echo "installed $(date +%H:%M:%S)"
