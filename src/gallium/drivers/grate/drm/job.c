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

/* Legacy Tegra DRM UAPI job submission (DRM_TEGRA_SUBMIT). */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>

#include "util/u_math.h"
#include "grate_bo.h"
#include "grate_device.h"
#include "tegra_private.h"

int
drm_tegra_job_new(struct drm_tegra_channel *channel,
                  struct drm_tegra_job **jobp)
{
    struct drm_tegra_job *job;

    job = calloc(1, sizeof(*job));
    if (!job)
        return -ENOMEM;

    job->page_size = sysconf(_SC_PAGESIZE);
    job->channel = channel;
    job->syncpt.id = channel->syncpt;

    *jobp = job;

    return 0;
}

int
drm_tegra_job_free(struct drm_tegra_job *job)
{
    if (!job)
        return -EINVAL;

    if (job->pushbuf)
        drm_tegra_pushbuf_free(job->pushbuf);

    if (job->cmdbuf_bo)
        grate_bo_unref(job->cmdbuf_bo);

    free(job->relocs);
    free(job);

    return 0;
}

int
drm_tegra_job_get_pushbuf(struct drm_tegra_job *job,
                          struct drm_tegra_pushbuf **pushbufp)
{
    struct drm_tegra_pushbuf *pushbuf;

    if (!job->pushbuf) {
        pushbuf = calloc(1, sizeof(*pushbuf));
        if (!pushbuf)
            return -ENOMEM;

        pushbuf->job = job;

        pushbuf->start = calloc(1, job->page_size);
        if (!pushbuf->start) {
            free(pushbuf);
            return -ENOMEM;
        }

        pushbuf->end = pushbuf->start + job->page_size / 4;
        pushbuf->ptr = pushbuf->start;

        job->pushbuf = pushbuf;
    }

    *pushbufp = job->pushbuf;

    return 0;
}

/*
 * The legacy SUBMIT ioctl gathers command words out of a GEM object, so the
 * pushbuffer has to be staged into one. The BO is sized to the job and kept on
 * the job until it is freed, since the kernel reads it asynchronously.
 */
static int
drm_tegra_job_stage_cmdbuf(struct drm_tegra_job *job)
{
    struct grate_device *drm = job->channel->drm;
    uint32_t size = align(job->words * 4, job->page_size);
    void *ptr;
    int err;

    if (job->cmdbuf_bo && job->cmdbuf_bo->size < size) {
        grate_bo_unref(job->cmdbuf_bo);
        job->cmdbuf_bo = NULL;
    }

    if (!job->cmdbuf_bo) {
        job->cmdbuf_bo = grate_bo_alloc(drm, size, 0);
        if (!job->cmdbuf_bo)
            return -ENOMEM;
    }

    err = grate_bo_map(job->cmdbuf_bo, &ptr);
    if (err < 0)
        return err;

    memcpy(ptr, job->pushbuf->start, job->words * 4);

    return 0;
}

int
drm_tegra_job_submit(struct drm_tegra_job *job, struct drm_tegra_fence *fence)
{
    struct drm_tegra_channel *channel = job->channel;
    struct grate_device *drm = channel->drm;
    struct drm_tegra_reloc *relocs = NULL;
    struct drm_tegra_cmdbuf cmdbuf;
    struct drm_tegra_syncpt syncpt;
    struct drm_tegra_submit args;
    unsigned int i;
    int err;

    if (!job->words)
        return -EINVAL;

    err = drm_tegra_job_stage_cmdbuf(job);
    if (err < 0)
        return err;

    memset(&cmdbuf, 0, sizeof(cmdbuf));
    cmdbuf.handle = job->cmdbuf_bo->handle;
    cmdbuf.offset = 0;
    cmdbuf.words = job->words;

    if (job->num_relocs) {
        relocs = calloc(job->num_relocs, sizeof(*relocs));
        if (!relocs)
            return -ENOMEM;

        /*
         * Order matters: the firewall walks the gather and consumes one reloc
         * per address register it sees, so these must stay in emit order.
         */
        for (i = 0; i < job->num_relocs; i++) {
            relocs[i].cmdbuf.handle = job->cmdbuf_bo->handle;
            relocs[i].cmdbuf.offset = job->relocs[i].gather_offset_words * 4;
            relocs[i].target.handle = job->relocs[i].target->handle;
            relocs[i].target.offset = job->relocs[i].target_offset;
            relocs[i].shift = job->relocs[i].shift;
        }
    }

    memset(&syncpt, 0, sizeof(syncpt));
    syncpt.id = job->syncpt.id;
    syncpt.incrs = job->syncpt.increments;

    memset(&args, 0, sizeof(args));
    args.context = channel->context;
    args.num_syncpts = 1;
    args.num_cmdbufs = 1;
    args.num_relocs = job->num_relocs;
    args.num_waitchks = 0;
    args.waitchk_mask = 0;
    args.timeout = 1000;
    args.syncpts = (uintptr_t)&syncpt;
    args.cmdbufs = (uintptr_t)&cmdbuf;
    args.relocs = (uintptr_t)relocs;
    args.waitchks = 0;

    err = drmCommandWriteRead(drm->fd, DRM_TEGRA_SUBMIT, &args, sizeof(args));

    free(relocs);

    if (err < 0)
        return err;

    job->syncpt.fence = args.fence;

    if (fence) {
        fence->drm = drm;
        fence->syncpt = job->syncpt.id;
        fence->value = job->syncpt.fence;
    }

    return 0;
}

int
drm_tegra_job_wait(struct drm_tegra_job *job, unsigned long timeout)
{
    struct drm_tegra_fence fence;

    fence.drm = job->channel->drm;
    fence.syncpt = job->syncpt.id;
    fence.value = job->syncpt.fence;

    return drm_tegra_fence_wait(&fence, timeout);
}
