/*
 * Copyright © 2012, 2013 Thierry Reding
 * Copyright © 2013 Erik Faye-Lund
 * Copyright © 2014 NVIDIA Corporation
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

#ifndef __DRM_TEGRA_PRIVATE_H__
#define __DRM_TEGRA_PRIVATE_H__ 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drm-uapi/tegra_drm.h"
#include "tegra.h"

enum host1x_class {
    HOST1X_CLASS_HOST1X = 0x01,
    HOST1X_CLASS_GR2D = 0x51,
    HOST1X_CLASS_GR2D_SB = 0x52,
    HOST1X_CLASS_VIC = 0x5d,
    HOST1X_CLASS_GR3D = 0x60,
};

struct drm_tegra_channel {
    struct grate_device *drm;
    enum host1x_class class;
    uint32_t capabilities;
    unsigned int version;
    uint64_t context;

    unsigned int cond_shift;

    /* legacy UAPI: one syncpoint is bound to the channel at open time */
    uint32_t syncpt;
};

/*
 * The legacy UAPI has no notion of a channel mapping: relocations name the
 * target GEM handle directly. A mapping is therefore just a pinned reference
 * to the BO, kept so drm_tegra_pushbuf_relocate() can reach its handle.
 */
struct drm_tegra_mapping {
    struct drm_tegra_channel *channel;
    struct grate_bo *bo;
    uint32_t flags;
};

struct drm_tegra_pushbuf {
    struct drm_tegra_job *job;

    uint32_t *start;
    uint32_t *end;
    uint32_t *ptr;
};

void drm_tegra_pushbuf_free(struct drm_tegra_pushbuf *pushbuf);

/* recorded at emit time, converted to struct drm_tegra_reloc at submit */
struct drm_tegra_legacy_reloc {
    uint32_t gather_offset_words;
    struct grate_bo *target;
    uint64_t target_offset;
    uint32_t shift;
};

struct drm_tegra_job {
    struct drm_tegra_channel *channel;
    struct drm_tegra_pushbuf *pushbuf;
    size_t page_size;

    struct drm_tegra_legacy_reloc *relocs;
    unsigned int num_relocs;

    /*
     * The legacy SUBMIT ioctl gathers from a GEM object, not from a userspace
     * pointer, so the pushbuf is copied into this BO at submit time. It has to
     * outlive the submit, so it is released in drm_tegra_job_free().
     */
    struct grate_bo *cmdbuf_bo;
    unsigned int words;

    struct {
        uint32_t id;
        uint32_t increments;
        uint32_t fence;
    } syncpt;
};

struct drm_tegra_syncpoint {
    struct grate_device *drm;
    uint32_t id;
};

#endif /* __DRM_TEGRA_PRIVATE_H__ */
