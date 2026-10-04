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

/* Legacy Tegra DRM UAPI pushbuffer: relocations are recorded against GEM
 * handles and resolved into struct drm_tegra_reloc at submit time. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_math.h"
#include "grate_bo.h"
#include "tegra_private.h"

#define HOST1X_OPCODE_NONINCR(offset, count) \
    ((0x2 << 28) | (((offset) & 0xfff) << 16) | ((count) & 0xffff))

static inline unsigned int
drm_tegra_pushbuf_get_offset(struct drm_tegra_pushbuf *pushbuf, uint32_t *ptr)
{
    return ptr - pushbuf->start;
}

void
drm_tegra_pushbuf_free(struct drm_tegra_pushbuf *pushbuf)
{
    if (pushbuf->start)
        free(pushbuf->start);

    free(pushbuf);
}

int
drm_tegra_pushbuf_begin(struct drm_tegra_pushbuf *pushbuf,
                        unsigned int words, uint32_t **ptrp)
{
    struct drm_tegra_job *job = pushbuf->job;
    unsigned long offset;
    size_t size;
    void *ptr;

    if (pushbuf->ptr + words >= pushbuf->end) {
        words = pushbuf->end - pushbuf->start + words;
        size = align(words * 4, job->page_size);
        offset = pushbuf->ptr - pushbuf->start;

        ptr = realloc(pushbuf->start, size);
        if (!ptr)
            return -ENOMEM;

        pushbuf->start = ptr;
        pushbuf->end = pushbuf->start + size / 4;
        pushbuf->ptr = pushbuf->start + offset;
    }

    if (ptrp)
        *ptrp = pushbuf->ptr;

    return 0;
}

int
drm_tegra_pushbuf_end(struct drm_tegra_pushbuf *pushbuf, uint32_t *ptr)
{
    pushbuf->ptr = ptr;

    /*
     * The word count handed to the kernel must match exactly what was pushed:
     * the host1x firewall walks the gather word by word, and a trailing zero
     * decodes as SETCL to class 0x0, which gr3d rejects.
     */
    pushbuf->job->words = ptr - pushbuf->start;

    return 0;
}

int
drm_tegra_pushbuf_wait(struct drm_tegra_pushbuf *pushbuf,
                       struct drm_tegra_syncpoint *syncpt,
                       uint32_t value)
{
    /*
     * Would need a class switch to HOST1X inside the gather, which this
     * kernel's firewall refuses for a gr3d job. Unused by the driver.
     */
    return -ENOSYS;
}

int
drm_tegra_pushbuf_relocate(struct drm_tegra_pushbuf *pushbuf, uint32_t **ptrp,
                           struct drm_tegra_mapping *target,
                           unsigned long offset, unsigned int shift,
                           uint32_t flags)
{
    struct drm_tegra_job *job = pushbuf->job;
    struct drm_tegra_legacy_reloc *relocs, *reloc;
    size_t size;

    if (!target || !target->bo)
        return -EINVAL;

    size = (job->num_relocs + 1) * sizeof(*relocs);

    relocs = realloc(job->relocs, size);
    if (!relocs)
        return -ENOMEM;

    reloc = &relocs[job->num_relocs];
    memset(reloc, 0, sizeof(*reloc));

    reloc->gather_offset_words = drm_tegra_pushbuf_get_offset(pushbuf, *ptrp);
    reloc->target = target->bo;
    reloc->target_offset = offset;
    reloc->shift = shift;

    /* patched by the kernel; must still occupy its slot in the stream */
    *(*ptrp)++ = 0xdeadbeef;

    job->relocs = relocs;
    job->num_relocs++;

    return 0;
}

int
drm_tegra_pushbuf_sync(struct drm_tegra_pushbuf *pushbuf,
                       struct drm_tegra_syncpoint *syncpt,
                       unsigned int count)
{
    struct drm_tegra_job *job = pushbuf->job;

    job->syncpt.increments += count;
    job->syncpt.id = syncpt->id;

    return 0;
}

int
drm_tegra_pushbuf_sync_cond(struct drm_tegra_pushbuf *pushbuf, uint32_t **ptrp,
                            struct drm_tegra_syncpoint *syncpt,
                            enum drm_tegra_sync_cond cond)
{
    struct drm_tegra_channel *channel = pushbuf->job->channel;

    if (cond >= DRM_TEGRA_SYNC_COND_MAX)
        return -EINVAL;

    *(*ptrp)++ = HOST1X_OPCODE_NONINCR(0x0, 0x1);
    *(*ptrp)++ = cond << channel->cond_shift | syncpt->id;

    return drm_tegra_pushbuf_sync(pushbuf, syncpt, 1);
}
