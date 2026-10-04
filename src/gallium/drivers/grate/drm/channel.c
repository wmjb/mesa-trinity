/*
 * Copyright © 2012, 2013 Thierry Reding
 * Copyright © 2013 Erik Faye-Lund
 * Copyright © 2014-2021 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Legacy (grate/opentegra) Tegra DRM UAPI backend.
 *
 * The kernel on Tegra20/30/114 exposes DRM_TEGRA_OPEN_CHANNEL / GET_SYNCPT /
 * SUBMIT rather than the CHANNEL and SYNCPOINT ioctls of the newer UAPI.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>

#include "grate_bo.h"
#include "grate_device.h"
#include "tegra_private.h"

int
drm_tegra_channel_open(struct grate_device *drm,
                       enum drm_tegra_class client,
                       struct drm_tegra_channel **channelp)
{
    struct drm_tegra_open_channel args;
    struct drm_tegra_get_syncpt syncpt_args;
    struct drm_tegra_channel *channel;
    enum host1x_class class;
    int err;

    if (!drm || !channelp)
        return -EINVAL;

    switch (client) {
    case DRM_TEGRA_HOST1X:
        class = HOST1X_CLASS_HOST1X;
        break;

    case DRM_TEGRA_GR2D:
        class = HOST1X_CLASS_GR2D;
        break;

    case DRM_TEGRA_GR3D:
        class = HOST1X_CLASS_GR3D;
        break;

    case DRM_TEGRA_VIC:
        class = HOST1X_CLASS_VIC;
        break;

    default:
        return -EINVAL;
    }

    channel = calloc(1, sizeof(*channel));
    if (!channel)
        return -ENOMEM;

    channel->drm = drm;
    channel->class = class;

    memset(&args, 0, sizeof(args));
    args.client = class;

    err = drmCommandWriteRead(drm->fd, DRM_TEGRA_OPEN_CHANNEL, &args,
                              sizeof(args));
    if (err < 0) {
        free(channel);
        return err;
    }

    channel->context = args.context;

    /* the legacy UAPI binds a single syncpoint to the channel */
    memset(&syncpt_args, 0, sizeof(syncpt_args));
    syncpt_args.context = args.context;
    syncpt_args.index = 0;

    err = drmCommandWriteRead(drm->fd, DRM_TEGRA_GET_SYNCPT, &syncpt_args,
                              sizeof(syncpt_args));
    if (err < 0) {
        drm_tegra_channel_close(channel);
        return err;
    }

    channel->syncpt = syncpt_args.id;

    /*
     * Every SoC that speaks this UAPI is host1x01/host1x02 (Tegra20, Tegra30,
     * Tegra114), where the syncpoint condition sits at bit 8 of the
     * SYNCPT_INCR register. host1x04+ moved it to bit 10, but those SoCs use
     * the newer UAPI and never reach this backend.
     */
    channel->cond_shift = 8;
    channel->version = 0x30;
    channel->capabilities = 0;

    *channelp = channel;

    return 0;
}

int
drm_tegra_channel_close(struct drm_tegra_channel *channel)
{
    struct drm_tegra_close_channel args;
    struct grate_device *drm;

    if (!channel)
        return -EINVAL;

    drm = channel->drm;

    memset(&args, 0, sizeof(args));
    args.context = channel->context;

    drmCommandWriteRead(drm->fd, DRM_TEGRA_CLOSE_CHANNEL, &args, sizeof(args));

    free(channel);

    return 0;
}

unsigned int
drm_tegra_channel_get_version(struct drm_tegra_channel *channel)
{
    return channel->version;
}

int
drm_tegra_channel_map(struct drm_tegra_channel *channel, struct grate_bo *bo,
                      uint32_t flags, struct drm_tegra_mapping **mapp)
{
    struct drm_tegra_mapping *map;

    if (!channel || !bo || !mapp)
        return -EINVAL;

    map = calloc(1, sizeof(*map));
    if (!map)
        return -ENOMEM;

    map->channel = channel;
    /*
     * No reference is taken: the mapping is cached in bo->channel_maps, so the
     * BO owns it and referencing back would be a cycle. The BO therefore always
     * outlives its mappings.
     */
    map->bo = bo;
    map->flags = flags;

    *mapp = map;

    return 0;
}

int
drm_tegra_channel_unmap(struct drm_tegra_mapping *map)
{
    if (!map)
        return -EINVAL;

    free(map);

    return 0;
}
