#ifndef GRATE_CONTEXT_H
#define GRATE_CONTEXT_H

#include "util/slab.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"

#include "grate_common.h"
#include "grate_device.h"
#include "grate_state.h"
#include "grate_stream.h"
#include "tgr_3d.xml.h"

struct grate_framebuffer_state {
   struct pipe_framebuffer_state base;
   int num_rts;
   bool scanout;      /* a render target is being scanned out */
   struct grate_bo *rt_bos[REG_TGR3D_GLOBAL_SURFDESC_LENGTH];
   uint32_t rt_params[REG_TGR3D_GLOBAL_SURFDESC_LENGTH];
   uint32_t rt_offset[PIPE_MAX_COLOR_BUFS + 1];
   uint32_t rt_mask;

   /* surface slot holding the depth buffer, or -1 when there is none */
   int zs_index;

   /* surface slot of colour target 0 */
   unsigned rt_base;
};

struct grate_channel {
   struct grate_context *context;
   struct grate_stream stream;
};

#define GRATE_MAX_SAMPLERS 16

struct grate_context {
   struct pipe_context base;

   struct grate_device *drm;
   struct grate_channel *gr2d;
   struct grate_channel *gr3d;

   struct grate_framebuffer_state framebuffer;

   struct slab_child_pool transfer_pool;

   struct grate_vertex_state *vs;
   struct grate_vertexbuf_state vbs;
   struct pipe_constant_buffer constant_buffer[PIPE_MAX_CONSTANT_BUFFERS]; // ??, stolen from other drivers but no idea

   struct grate_zsa_state *zsa;
   struct pipe_blend_state *blend;
   struct grate_rasterizer_state *rast;

   struct grate_vertex_shader_state *vshader;
   struct grate_fragment_shader_state *fshader;

   struct pipe_sampler_view *sampler_views[GRATE_MAX_SAMPLERS];
   struct pipe_sampler_state *samplers[GRATE_MAX_SAMPLERS];
   unsigned num_sampler_views;
   unsigned num_samplers;

   uint32_t no_scissor[3];
   uint32_t viewport[10];
   uint32_t guardband[4];
   bool y_invert;
   struct pipe_scissor_state scissor;
};

static inline struct grate_context *
grate_context(struct pipe_context *context)
{
   return (struct grate_context *)context;
}

void
grate_context_flush_streams(struct grate_context *context);

struct pipe_context *
grate_screen_context_create(struct pipe_screen *pscreen,
                            void *priv, unsigned flags);

#endif
