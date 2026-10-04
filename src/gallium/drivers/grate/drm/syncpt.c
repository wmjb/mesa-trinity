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

/* Legacy Tegra DRM UAPI: syncpoints are owned by the channel, not allocated. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <xf86drm.h>

#include "grate_device.h"
#include "tegra_private.h"

int
drm_tegra_syncpoint_new(struct drm_tegra_channel *channel,
                        struct drm_tegra_syncpoint **syncptp)
{
    struct drm_tegra_syncpoint *syncpt;

    if (!channel || !syncptp)
        return -EINVAL;

    syncpt = calloc(1, sizeof(*syncpt));
    if (!syncpt)
        return -ENOMEM;

    /*
     * There is no SYNCPOINT_ALLOCATE here: the syncpoint was handed out by
     * DRM_TEGRA_GET_SYNCPT when the channel was opened, and is released with
     * the channel.
     */
    syncpt->drm = channel->drm;
    syncpt->id = channel->syncpt;

    *syncptp = syncpt;

    return 0;
}

int
drm_tegra_syncpoint_free(struct drm_tegra_syncpoint *syncpt)
{
    if (!syncpt)
        return -EINVAL;

    free(syncpt);

    return 0;
}

int
drm_tegra_fence_wait(struct drm_tegra_fence *fence, unsigned long timeout)
{
    struct drm_tegra_syncpt_wait args;
    int err;

    if (!fence)
        return -EINVAL;

    memset(&args, 0, sizeof(args));
    args.id = fence->syncpt;
    args.thresh = fence->value;

    /*
     * The caller's timeout is in nanoseconds, the legacy ioctl takes
     * milliseconds. Round up, so a sub-millisecond request still waits a
     * whole tick rather than collapsing to zero and returning immediately.
     */
    if (timeout == 0)
        args.timeout = 0;
    else
        args.timeout = (timeout + 999999ul) / 1000000ul;

    do {
        err = drmCommandWriteRead(fence->drm->fd, DRM_TEGRA_SYNCPT_WAIT, &args,
                                  sizeof(args));
    } while (err == -EINTR || err == -EAGAIN);

    if (err < 0)
        return err;

    return 0;
}
