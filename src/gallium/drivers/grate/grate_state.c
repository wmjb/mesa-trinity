#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "util/format/u_format.h"
#include "util/u_bitcast.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_framebuffer.h"

#include "fp/fpir.h"
#include "grate_common.h"
#include "grate_context.h"
#include "grate_program.h"
#include "grate_resource.h"
#include "grate_state.h"

#include "host1x01_hardware.h"

static void
grate_set_sample_mask(struct pipe_context *pcontext,
                      unsigned int sample_mask)
{
   grate_unimplemented();
}

static void
grate_set_constant_buffer(struct pipe_context *pcontext, enum mesa_shader_stage shader,
                          uint index, const struct pipe_constant_buffer *buffer)
{
   struct grate_context *context = grate_context(pcontext);

   assert(index == 0);
   assert(!buffer || buffer->user_buffer);

   util_copy_constant_buffer(&context->constant_buffer[shader], buffer);
}

static void grate_add_render_target(struct grate_context *context,
                                    const struct pipe_surface *ref,
                                    bool is_depth) 
{
   if (!ref->texture) {
      fprintf(stderr, "%s: texture at %p null!", __func__, ref);
      assert(0);
      return;
   }
   if (context->framebuffer.num_rts >= context->base.screen->caps.max_render_targets) {
      fprintf(stderr, "%s: Reached max render targets!", __func__);
      assert(0);
      return;
   }
   struct grate_resource *res = grate_resource(ref->texture);
   uint32_t rt_params;

   if (ref->texture->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET))
      context->framebuffer.scanout = true;
   
   if (grate_debug & GRATE_DEBUG_TRACE)
      fprintf(stderr, "GRATE RT: pipe_format=%s hw_format=%d\n",
              util_format_short_name(ref->format), res->format);
   unsigned rlvl = MIN2(ref->level, (unsigned)(GRATE_MAX_MIP_LEVELS - 1));
   unsigned rpitch = res->level_pitch[rlvl] ? res->level_pitch[rlvl] : res->pitch;

int hw_fmt = grate_pixel_format(ref->format);
   if (hw_fmt <  0) {
      fprintf(stderr, "%s: unsupported render target format: %s\n",
              __func__, util_format_short_name(ref->format));
      assert(0);
   }

   rt_params  = TGR3D_GLOBAL_SURFDESC_SURF_FORMAT(hw_fmt);
   rt_params |= TGR3D_GLOBAL_SURFDESC_ARRAY_STRIDE(rpitch);
   rt_params |= TGR3D_GLOBAL_SURFDESC_STRUCTURE(res->tiled);
   
   /*
    * Rendering into a mip level has to land at that level's offset. Without
    * this every level Mesa generates was written over level 0, so a mip chain
    * had storage but no contents and minification stayed aliased.
    */
   unsigned lvl = MIN2(ref->level, (unsigned)(GRATE_MAX_MIP_LEVELS - 1));

   context->framebuffer.rt_params[context->framebuffer.num_rts] = rt_params;
   context->framebuffer.rt_bos[context->framebuffer.num_rts] = res->bo;
   context->framebuffer.rt_offset[context->framebuffer.num_rts] =
      res->level_offset[lvl];

   /*
    * The depth buffer needs a surface slot of its own for its address and
    * format, but it must not appear in DW_ST_ENABLE: that is the mask of
    * surfaces the fragment stage stores colour into, and listing depth there
    * has the shader's colour written over the depth values.
    */
   if (is_depth)
      context->framebuffer.zs_index = context->framebuffer.num_rts;
   else
      context->framebuffer.rt_mask |= 1 << context->framebuffer.num_rts;

   context->framebuffer.num_rts++;
}

static void
grate_set_framebuffer_state(struct pipe_context *pcontext,
                            const struct pipe_framebuffer_state *framebuffer)
{
   grate_context(pcontext)->framebuffer.scanout = false;
   struct grate_context *context = grate_context(pcontext);
   struct pipe_framebuffer_state *cso = &context->framebuffer.base;
   context->framebuffer.rt_mask = 0;
   context->framebuffer.num_rts = 0;
   context->framebuffer.zs_index = -1;
   context->framebuffer.rt_base = 0;

   util_copy_framebuffer_state(cso, framebuffer);


   context->framebuffer.rt_base = context->framebuffer.num_rts;

   for (unsigned int i = 0; i < framebuffer->nr_cbufs; i++) {
      grate_add_render_target(context, &framebuffer->cbufs[i], false);
   }

   if (framebuffer->zsbuf.texture) {
      grate_add_render_target(context, &framebuffer->zsbuf, true);
   }


   /* prepare the scissor-registers for the non-scissor case */
   context->no_scissor[0]  = host1x_opcode_incr(REG_TGR3D_SU_SCISSOR_X, 2);
   context->no_scissor[1]  = TGR3D_SU_SCISSOR_X_MIN(0);
   context->no_scissor[1] |= TGR3D_SU_SCISSOR_X_MAX(framebuffer->width);
   context->no_scissor[2]  = TGR3D_SU_SCISSOR_Y_MIN(0);
   context->no_scissor[2] |= TGR3D_SU_SCISSOR_Y_MAX(framebuffer->height);
}

static void
grate_set_polygon_stipple(struct pipe_context *pcontext,
                          const struct pipe_poly_stipple *stipple)
{
   grate_unimplemented();
}

static void
grate_set_scissor_states(struct pipe_context *pcontext,
                         unsigned start_slot,
                         unsigned num_scissors,
                         const struct pipe_scissor_state * scissors)
{
   struct grate_context *context = grate_context(pcontext);

   assert(num_scissors == 1);
   assert(start_slot == 0);

   context->scissor = scissors[0];
}

static void
grate_set_viewport_states(struct pipe_context *pcontext,
                          unsigned start_slot,
                          unsigned num_viewports,
                          const struct pipe_viewport_state *viewports)
{
   struct grate_context *context = grate_context(pcontext);
   static const float zeps = powf(2.0f, -21);

   /* Determine Z hardware integer range from active depth surface format */
   struct pipe_framebuffer_state *fb = &context->framebuffer.base;
   float hw_scale = 65535.0f; /* default 16-bit z16_unorm */

   if (fb->zsbuf.texture) {
      if (fb->zsbuf.format == PIPE_FORMAT_Z24_UNORM_S8_UINT ||
          fb->zsbuf.format == PIPE_FORMAT_Z24X8_UNORM) {
         hw_scale = (context->drm->soc_id == DRM_TEGRA_SOC_T114) ? 16777215.0f : 1048575.0f;
      } else if (fb->zsbuf.format == PIPE_FORMAT_Z16_UNORM) {
         hw_scale = 65535.0f;
      }
   }

   assert(num_viewports == 1);
   assert(start_slot == 0);

   float z_scale = viewports[0].scale[2] * hw_scale;
   float z_bias  = viewports[0].translate[2] * hw_scale;

   context->viewport[0] = host1x_opcode_incr(REG_TGR3D_SU_VIEWPORT_X, 6);
   context->viewport[1] = u_bitcast_f2u(viewports[0].translate[0] * 16.0f);
   context->viewport[2] = u_bitcast_f2u(viewports[0].translate[1] * 16.0f);
   context->viewport[3] = u_bitcast_f2u(viewports[0].translate[2] - zeps);
   context->viewport[4] = u_bitcast_f2u(viewports[0].scale[0] * 16.0f);
   context->viewport[5] = u_bitcast_f2u(viewports[0].scale[1] * 16.0f);
   context->viewport[6] = u_bitcast_f2u(viewports[0].scale[2] - zeps);

   float d_near = viewports[0].translate[2] - viewports[0].scale[2];
   float d_far  = viewports[0].translate[2] + viewports[0].scale[2];

   uint32_t depth_near = CLAMP(d_near * hw_scale, 0.0f, (float)hw_scale);
   uint32_t depth_far  = CLAMP(d_far  * hw_scale, 0.0f, (float)hw_scale);

   context->viewport[7] = host1x_opcode_incr(REG_TGR3D_QR_Z_MIN, 2);
   context->viewport[8] = MIN2(depth_near, depth_far);
   context->viewport[9] = MAX2(depth_near, depth_far);

   assert(viewports[0].scale[0] >= 0.0f);
   float max_x = fabs(viewports[0].translate[0]);
   float max_y = fabs(viewports[0].translate[1]);
   float scale_x = viewports[0].scale[0];
   float scale_y = fabs(viewports[0].scale[1]);
   context->guardband[0] = host1x_opcode_incr(REG_TGR3D_SU_GUARDBAND_W, 3);
   context->guardband[1] = u_bitcast_f2u((3967 - max_x) / scale_x);
   context->guardband[2] = u_bitcast_f2u((3967 - max_y) / scale_y);
   context->guardband[3] = u_bitcast_f2u(6.99);

   /* Flag VPM dirty if Y-inversion state toggles */
   bool old_y_invert = context->y_invert;
   context->y_invert = viewports[0].scale[1] < 0.0f;
   if (old_y_invert != context->y_invert)
      context->dirty |= GRATE_DIRTY_VPM;

   if (getenv("GRATE_VP_TRACE"))
      fprintf(stderr, "grate: viewport translate=(%.1f,%.1f) scale=(%.1f,%.1f) y_invert=%d hw_scale=%.0f z_bias=%.1f z_scale=%.1f\n",
              viewports[0].translate[0], viewports[0].translate[1],
              viewports[0].scale[0], viewports[0].scale[1], context->y_invert, hw_scale, z_bias, z_scale);
}

static void
grate_set_vertex_buffers(struct pipe_context *pcontext,
                         unsigned count,
                         const struct pipe_vertex_buffer *buffer)
{
   struct grate_context *context = grate_context(pcontext);
   struct grate_vertexbuf_state *vbs = &context->vbs;

   util_set_vertex_buffers_mask(vbs->vb, &vbs->enabled, buffer, count);
   vbs->count = util_last_bit(vbs->enabled);
}


static void
grate_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader,
                        unsigned start_slot, unsigned num_views,
                        unsigned unbind_num_trailing_slots,
                        struct pipe_sampler_view **views)
{
   struct grate_context *context = grate_context(pctx);

   /* only the fragment stage can sample on this hardware */
   if (shader != MESA_SHADER_FRAGMENT)
      return;

   for (unsigned i = 0; i < num_views; ++i) {
      unsigned slot = start_slot + i;
      if (slot >= GRATE_MAX_SAMPLERS)
         break;
      pipe_sampler_view_reference(&context->sampler_views[slot],
                                  views ? views[i] : NULL);
   }

   for (unsigned i = 0; i < unbind_num_trailing_slots; ++i) {
      unsigned slot = start_slot + num_views + i;
      if (slot >= GRATE_MAX_SAMPLERS)
         break;
      pipe_sampler_view_reference(&context->sampler_views[slot], NULL);
   }

   context->num_sampler_views = 0;
   for (unsigned i = 0; i < GRATE_MAX_SAMPLERS; ++i)
      if (context->sampler_views[i])
         context->num_sampler_views = i + 1;
}

static void
grate_set_blend_color(struct pipe_context *pctx,
                      const struct pipe_blend_color *blend_color)
{
   grate_unimplemented();
}

static void
grate_set_stencil_ref(struct pipe_context *pctx,
                      const struct pipe_stencil_ref stencil_ref)
{
   grate_unimplemented();
}

static void
grate_fp_state_bind(struct pipe_context *pctx, void *hwcso)
{
   grate_unimplemented();
}

static void
grate_vp_state_bind(struct pipe_context *pctx, void *hwcso)
{
   grate_unimplemented();
}

void
grate_context_state_init(struct pipe_context *pcontext)
{
   pcontext->set_blend_color = grate_set_blend_color;
   pcontext->set_stencil_ref = grate_set_stencil_ref;
   pcontext->bind_fs_state = grate_fp_state_bind;
   pcontext->bind_vs_state = grate_vp_state_bind;
   pcontext->set_sample_mask = grate_set_sample_mask;
   pcontext->set_constant_buffer = grate_set_constant_buffer;
   pcontext->set_framebuffer_state = grate_set_framebuffer_state;
   pcontext->set_polygon_stipple = grate_set_polygon_stipple;
   pcontext->set_scissor_states = grate_set_scissor_states;
   pcontext->set_viewport_states = grate_set_viewport_states;
   pcontext->set_sampler_views = grate_set_sampler_views;
   pcontext->set_vertex_buffers = grate_set_vertex_buffers;
}

static void *
grate_create_blend_state(struct pipe_context *pcontext,
                         const struct pipe_blend_state *template)
{
   struct pipe_blend_state *so = CALLOC_STRUCT(pipe_blend_state);
   if (!so)
      return NULL;

   *so = *template;

   return so;
}

static void
grate_bind_blend_state(struct pipe_context *pcontext, void *so)
{
   /*
    * Kept, but it changes nothing yet: there is no blending in the register
    * set at all - the data write stage offers a logic op and nothing else -
    * and the one destination read path the hardware does have (see
    * GLOBAL_MEMORY_OUTPUT_READS in grate_draw.c) delivers the destination
    * after the fragment program has run, so a shader cannot combine with it.
    * Everything therefore draws opaque.
    */
   grate_context(pcontext)->blend = so;
}

static void
grate_delete_blend_state(struct pipe_context *pcontext, void *so)
{
   FREE(so);
}

void
grate_context_blend_init(struct pipe_context *pcontext)
{
   pcontext->create_blend_state = grate_create_blend_state;
   pcontext->bind_blend_state = grate_bind_blend_state;
   pcontext->delete_blend_state = grate_delete_blend_state;
}

static void *
grate_create_sampler_state(struct pipe_context *pcontext,
            const struct pipe_sampler_state *template)
{
   struct pipe_sampler_state *so = CALLOC_STRUCT(pipe_sampler_state);
   if (!so)
      return NULL;

   *so = *template;

   return so;
}

static void
grate_bind_sampler_states(struct pipe_context *pcontext,
                          enum mesa_shader_stage shader,
                          unsigned start_slot, unsigned num_samplers,
                          void **samplers)
{
   struct grate_context *context = grate_context(pcontext);


   if (shader != MESA_SHADER_FRAGMENT)
      return;

   for (unsigned i = 0; i < num_samplers; ++i) {
      unsigned slot = start_slot + i;
      if (slot >= GRATE_MAX_SAMPLERS)
         break;
      context->samplers[slot] = samplers ? samplers[i] : NULL;
   }

   context->num_samplers = 0;
   for (unsigned i = 0; i < GRATE_MAX_SAMPLERS; ++i)
      if (context->samplers[i])
         context->num_samplers = i + 1;
}

static void
grate_delete_sampler_state(struct pipe_context *pcontext, void *so)
{
   FREE(so);
}

static struct pipe_sampler_view *
grate_create_sampler_view(struct pipe_context *pcontext,
                          struct pipe_resource *resource,
                          const struct pipe_sampler_view *template)
{
   struct pipe_sampler_view *so = CALLOC_STRUCT(pipe_sampler_view);
   if (!so)
      return NULL;

   *so = *template;
   so->texture = NULL;
   pipe_resource_reference(&so->texture, resource);
   pipe_reference_init(&so->reference, 1);
   so->context = pcontext;

   return so;
}

static void
grate_sampler_view_destroy(struct pipe_context *pcontext,
                           struct pipe_sampler_view *pview)
{
   pipe_resource_reference(&pview->texture, NULL);
   FREE(pview);
}

void
grate_context_sampler_init(struct pipe_context *pcontext)
{
   grate_trace();
   pcontext->create_sampler_state = grate_create_sampler_state;
   pcontext->bind_sampler_states = grate_bind_sampler_states;
   pcontext->delete_sampler_state = grate_delete_sampler_state;
   pcontext->create_sampler_view = grate_create_sampler_view;
   pcontext->sampler_view_destroy = grate_sampler_view_destroy;
   pcontext->sampler_view_release = u_default_sampler_view_release;
}

static uint32_t
grate_cull_face(unsigned cull_face)
{
   switch (cull_face) {
   case PIPE_FACE_NONE:
      return 0;
   case PIPE_FACE_BACK:
      return 1; /* Hardware mode 1: Cull Back */
   case PIPE_FACE_FRONT:
      return 2; /* Hardware mode 2: Cull Front */
   case PIPE_FACE_FRONT_AND_BACK:
      return 3;
   }

   return 0;
}

static void *
grate_create_rasterizer_state(struct pipe_context *pcontext,
                              const struct pipe_rasterizer_state *template)
{
   struct grate_rasterizer_state *so = CALLOC_STRUCT(grate_rasterizer_state);
   if (!so)
      return NULL;

   so->base = *template;
   so->draw_params = TGR3D_IDX_SET_PRIM_FLAT_VTX(!template->flatshade_first);

   /* normal (y_invert = 0): front face is template->front_ccw */
   so->cull_face[0] = TGR3D_SU_PARAM_FRONT_FACE(template->front_ccw) |
                      TGR3D_SU_PARAM_CULL(grate_cull_face(template->cull_face));

   /* y-inverted (y_invert = 1): invert front face winding due to Y-flip */
   so->cull_face[1] = TGR3D_SU_PARAM_FRONT_FACE(!template->front_ccw) |
                      TGR3D_SU_PARAM_CULL(grate_cull_face(template->cull_face));

   return so;
}

static void
grate_bind_rasterizer_state(struct pipe_context *pcontext, void *so)
{
   grate_context(pcontext)->rast = so;
}

static void
grate_delete_rasterizer_state(struct pipe_context *pcontext, void *so)
{
   FREE(so);
}

void
grate_context_rasterizer_init(struct pipe_context *pcontext)
{
   grate_trace();
   pcontext->create_rasterizer_state = grate_create_rasterizer_state;
   pcontext->bind_rasterizer_state = grate_bind_rasterizer_state;
   pcontext->delete_rasterizer_state = grate_delete_rasterizer_state;
}

static int
grate_compare_func(enum pipe_compare_func func)
{
   switch (func) {
   case PIPE_FUNC_NEVER: return TGR3D_FUNC_NEVER;
   case PIPE_FUNC_LESS: return TGR3D_FUNC_LESS;
   case PIPE_FUNC_EQUAL: return TGR3D_FUNC_EQUAL;
   case PIPE_FUNC_LEQUAL: return TGR3D_FUNC_LEQUAL;
   case PIPE_FUNC_GREATER: return TGR3D_FUNC_GREATER;
   case PIPE_FUNC_NOTEQUAL: return TGR3D_FUNC_NOTEQUAL;
   case PIPE_FUNC_GEQUAL: return TGR3D_FUNC_GEQUAL;
   case PIPE_FUNC_ALWAYS: return TGR3D_FUNC_ALWAYS;
   default: UNREACHABLE("unknown pipe_compare_func");
   }
}

static void *
grate_create_zsa_state(struct pipe_context *pcontext,
                       const struct pipe_depth_stencil_alpha_state *template)
{
   struct grate_context *context = grate_context(pcontext);
   struct grate_zsa_state *so = CALLOC_STRUCT(grate_zsa_state);
   grate_trace();
   if (!so)
      return NULL;

   so->base = *template;

   uint32_t depth_test = 0;
   /*
    * Hardware depth is off unless GRATE_HW_DEPTH is set, because the depth
    * unit does not use the depth buffer. Measured: clearing the depth buffer
    * to 1.0 or to 0.0 makes no difference to the test, while the colours
    * written into surface 0 do - the unit reads and writes surface 0, the
    * colour buffer, whatever QR_Z_TEST's Z_SURF_PTR says. Sweeping that field,
    * every other bit of QR_Z_TEST, DW_ST_ENABLE, SURFOVERADDR, the OVERLAP
    * descriptor bit, GLOBAL_FLUSH and both depth formats changes nothing, and
    * putting the depth buffer in surface 0 instead stops colour reaching the
    * framebuffer at all.
    *
    * That is why every lit scene came out in alternating columns: the colour
    * buffer cleared to opaque black is BGRA 00 00 00 ff, which read as pairs
    * of 16 bit depths is 0x0000, 0xff00, 0x0000, ... so every second pixel
    * tested against 0.0 and failed. Enabling it also has the rasterizer write
    * depth over the left half of every colour row.
    *
    * Leaving it off costs correct occlusion, which matters to a 3D app but not
    * to a compositor, and buys a picture that is merely flat rather than
    * corrupt. The state is still translated so that turning the switch on is
    * all it takes to carry on investigating.
    */

   bool hw_depth = getenv("GRATE_HW_DEPTH") != NULL;

   bool z_enable = hw_depth && template->depth_enabled;
   bool z_write  = hw_depth && template->depth_enabled && template->depth_writemask;

   depth_test |= TGR3D_QR_Z_TEST_Z_FUNC(grate_compare_func(template->depth_func));
   depth_test |= TGR3D_QR_Z_TEST_Z_ENABLE(z_enable);
   depth_test |= TGR3D_QR_Z_TEST_QRAST_FB_WRITE(z_write);
   depth_test |= TGR3D_QR_Z_TEST_Z_CLAMP(TGR3D_Z_CLAMP_CLAMP);

   so->commands[0] = host1x_opcode_incr(REG_TGR3D_QR_Z_TEST, 1);
   so->commands[1] = depth_test;
   so->num_commands = 2;

   if (context->drm->soc_id == DRM_TEGRA_SOC_T114) {
      so->commands[2] = host1x_opcode_incr(0xe45, 1);
      so->commands[3] = depth_test;
      so->num_commands = 4;
   }

   return so;
}

static void
grate_bind_zsa_state(struct pipe_context *pcontext, void *so)
{
   grate_context(pcontext)->zsa = so;
}

static void
grate_delete_zsa_state(struct pipe_context *pcontext, void *so)
{
   FREE(so);
}

void
grate_context_zsa_init(struct pipe_context *pcontext)
{
   grate_trace();
   pcontext->create_depth_stencil_alpha_state = grate_create_zsa_state;
   pcontext->bind_depth_stencil_alpha_state = grate_bind_zsa_state;
   pcontext->delete_depth_stencil_alpha_state = grate_delete_zsa_state;
}

/*
 * Note: this does not include the stride, which needs to be mixed in later
 **/
static uint32_t
attrib_mode(const struct pipe_vertex_element *e)
{
   const struct util_format_description *desc = util_format_description(e->src_format);
   const int c = util_format_get_first_non_void_channel(e->src_format);
   uint32_t type, format;

   assert(!desc->is_mixed);
   assert(c >= 0);

   switch (desc->channel[c].type) {
   case UTIL_FORMAT_TYPE_UNSIGNED:
   case UTIL_FORMAT_TYPE_SIGNED:
      switch (desc->channel[c].size) {
      case 8:
         type = TGR3D_ATTR_FMT_U8;
         break;

      case 16:
         type = TGR3D_ATTR_FMT_U16;
         break;

      case 32:
         type = TGR3D_ATTR_FMT_U32;
         break;

      default:
         UNREACHABLE("invalid channel-size");
      }

      if (desc->channel[c].type == UTIL_FORMAT_TYPE_SIGNED)
         type += 2;

      if (desc->channel[c].normalized)
         type += 1;

      break;

   case UTIL_FORMAT_TYPE_FIXED:
      assert(desc->channel[c].size == 32);
      type = TGR3D_ATTR_FMT_X32;
      break;

   case UTIL_FORMAT_TYPE_FLOAT:
      assert(desc->channel[c].size == 32); /* TODO: float16 ? */
      type = TGR3D_ATTR_FMT_F32;
      break;

   default:
      UNREACHABLE("invalid channel-type");
   }

   format  = TGR3D_IDX_ATTRIBUTE_MODE_ATTR_FMT(type);
   format |= TGR3D_IDX_ATTRIBUTE_MODE_ATTR_SIZE(desc->nr_channels);
   return format;
}

static void *
grate_create_vertex_state(struct pipe_context *pcontext, unsigned int count,
                          const struct pipe_vertex_element *elements)
{
   unsigned int i;
   uint16_t mask = 0;
   struct grate_vertex_state *vtx = CALLOC_STRUCT(grate_vertex_state);
   if (!vtx)
      return NULL;

   for (i = 0; i < count; ++i) {
      const struct pipe_vertex_element *src = elements + i;
      struct grate_vertex_element *dst = vtx->elements + i;
      dst->attrib = attrib_mode(src);
      dst->buffer_index = src->vertex_buffer_index;
      dst->offset = src->src_offset;
      dst->stride = src->src_stride;
      mask |= 1 << i;
   }

   vtx->num_elements = count;
   vtx->mask = mask;

   return vtx;
}

static void
grate_bind_vertex_state(struct pipe_context *pcontext, void *so)
{
   grate_context(pcontext)->vs = so;
}

static void
grate_delete_vertex_state(struct pipe_context *pcontext, void *so)
{
   FREE(so);
}

static void
emit_attribs(struct grate_context *context, uint32_t **ptrp)
{
   unsigned int i;
   struct grate_stream *stream = &context->gr3d->stream;

   assert(context->vs);

   for (i = 0; i < context->vs->num_elements; ++i) {
      const struct pipe_vertex_buffer *vb;
      const struct grate_vertex_element *e = context->vs->elements + i;
      const struct grate_resource *r;

      assert(e->buffer_index < context->vbs.count);
      vb = context->vbs.vb + e->buffer_index;
      assert(!vb->is_user_buffer);
      r = grate_resource(vb->buffer.resource);

      uint32_t attrib = e->attrib;
      assert(e->stride < 1 << 24);
      attrib |= TGR3D_IDX_ATTRIBUTE_MODE_ATTR_STRIDE(e->stride);

      GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_IDX_ATTRIBUTE_BASE(i), 2));
      grate_stream_push_reloc(stream, ptrp, r->bo, vb->buffer_offset + e->offset);
      GRATE_PUSHBUF_WORD(*ptrp, attrib);
   }
}

static void
emit_render_targets(struct grate_context *context, uint32_t **ptrp)
{
   unsigned int i;
   struct grate_stream *stream = &context->gr3d->stream;
   const struct grate_framebuffer_state *fb = &context->framebuffer;

   *(*ptrp)++ = host1x_opcode_incr(REG_TGR3D_GLOBAL_SURFDESC(0), fb->num_rts);
   for (i = 0; i < fb->num_rts; ++i) {
      uint32_t rt_params = fb->rt_params[i];
      /* TODO: setup dither */
      /* rt_params |= TGR3D_GLOBAL_SURFDESC_DITHER(enable_dither); */
      *(*ptrp)++ = rt_params;
   }

   GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_GLOBAL_SURFADDR(0), fb->num_rts));
   for (i = 0; i < fb->num_rts; ++i) {
      grate_stream_push_reloc(stream, ptrp, fb->rt_bos[i], fb->rt_offset[i]);
   }


   GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_DW_ST_ENABLE, 1));
   GRATE_PUSHBUF_WORD(*ptrp, fb->rt_mask);
}

/*
 * Texture descriptors, following libgrate's grate_3d_set_texture_desc():
 * DESC_LO carries format and filter/wrap state, DESC_HI the dimensions, given
 * as log2 for power-of-two textures and literally otherwise.
 */
static void
emit_textures(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   unsigned num = context->num_sampler_views;

   if (num == 0)
      return;

   for (unsigned i = 0; i < num; ++i) {
      struct pipe_sampler_view *view = context->sampler_views[i];
      struct pipe_sampler_state *smp = context->samplers[i];

      if (!view)
         continue;

      struct grate_resource *res = grate_resource(view->texture);

      GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_TEX_TEXADDR(i), 1));
      grate_stream_push_reloc(stream, ptrp, res->bo, 0);

      unsigned width = view->texture->width0;
      unsigned height = view->texture->height0;
      int log2_width = util_logbase2(width);
      int log2_height = util_logbase2(height);
      bool pot = (width == (1u << log2_width)) && (height == (1u << log2_height));

      bool mag_linear = smp && smp->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
      bool min_linear = smp && smp->min_img_filter == PIPE_TEX_FILTER_LINEAR;
      bool mip_linear = smp && smp->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR;
      bool mipmapped  = smp && smp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE;

      enum pipe_format format = view->format;
      int hw_fmt = grate_pixel_format(format);
      if (hw_fmt < 0) {
         fprintf(stderr, "%s: unsupported texture format: %s\n",
                 __func__, util_format_short_name(format));
         assert(0);
      }

      uint32_t lo = TGR3D_TEX_TEXDESC_LO_SURF_FORMAT(hw_fmt);

      if (mag_linear)
         lo |= TGR3D_TEX_TEXDESC_LO_LERP_MAG;
      if (min_linear)
         lo |= TGR3D_TEX_TEXDESC_LO_LERP_MIN;
      if (smp) {
         if (smp->wrap_s == PIPE_TEX_WRAP_CLAMP_TO_EDGE)
            lo |= TGR3D_TEX_TEXDESC_LO_CLAMP_S(TGR3D_CLAMP_CLAMP);
         if (smp->wrap_t == PIPE_TEX_WRAP_CLAMP_TO_EDGE)
            lo |= TGR3D_TEX_TEXDESC_LO_CLAMP_T(TGR3D_CLAMP_CLAMP);
         if (smp->wrap_s == PIPE_TEX_WRAP_MIRROR_REPEAT)
            lo |= TGR3D_TEX_TEXDESC_LO_MIRROR_S(TGR3D_STATE_ENABLED);
         if (smp->wrap_t == PIPE_TEX_WRAP_MIRROR_REPEAT)
            lo |= TGR3D_TEX_TEXDESC_LO_MIRROR_T(TGR3D_STATE_ENABLED);
      }

      uint32_t hi = 0;

       /*
       * Only set NORMALIZE when requested by the sampler state.
       * Unnormalized coordinates (e.g. st/drawtex) require NORMALIZE to be 0
       * so texels are indexed directly by pixel offsets.
       */
      bool normalized = !smp || !smp->unnormalized_coords;
      if (normalized)
         hi |= TGR3D_TEX_TEXDESC_HI_NORMALIZE__MASK;

      hi |= TGR3D_TEX_TEXDESC_HI_BASE_LEVEL_ONLY__MASK;

      if (pot) {
         hi |= TGR3D_TEX_TEXDESC_HI_LOG2_WIDTH(log2_width);
         hi |= TGR3D_TEX_TEXDESC_HI_LOG2_HEIGHT(log2_height);
      } else {
         hi |= GRATE_TEXDESC_HI_NOT_POW2;
         hi |= TGR3D_TEX_TEXDESC_HI_WIDTH(width);
         hi |= TGR3D_TEX_TEXDESC_HI_HEIGHT(height);
      }

      if (grate_debug & GRATE_DEBUG_TRACE)
         fprintf(stderr, "GRATE TEX%u: %s %ux%u pitch=%u hw_fmt=%d pot=%d "
                         "lo=%08x hi=%08x smp=%p\n",
                 i, util_format_short_name(view->texture->format),
                 width, height, res->pitch, res->format, pot, lo, hi,
                 (void *)smp);

      GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_TEX_TEXDESC_LO(i), 2));
      GRATE_PUSHBUF_WORD(*ptrp, lo);
      GRATE_PUSHBUF_WORD(*ptrp, hi);
   }
}

static void
emit_scissor(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;

   /*
    * Without this the rasterizer covered the whole target whatever was asked
    * for, so a compositor repainting only what changed drew every surface
    * across the entire screen. The scissor arrives in the same window space
    * the viewport puts fragments in, so it needs no flip of its own.
    */
   if (context->rast && context->rast->base.scissor) {
      const struct pipe_scissor_state *s = &context->scissor;
      uint32_t words[3];

      words[0] = host1x_opcode_incr(REG_TGR3D_SU_SCISSOR_X, 2);
      words[1] = TGR3D_SU_SCISSOR_X_MIN(s->minx) |
                 TGR3D_SU_SCISSOR_X_MAX(s->maxx);
      words[2] = TGR3D_SU_SCISSOR_Y_MIN(s->miny) |
                 TGR3D_SU_SCISSOR_Y_MAX(s->maxy);

      grate_stream_push_words(stream, ptrp, words, 3, 0);
      return;
   }

   grate_stream_push_words(stream, ptrp, context->no_scissor, 3, 0);
}

static void
emit_viewport(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;

   grate_stream_push_words(stream, ptrp, context->viewport, 10, 0);
}

static void
emit_guardband(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   grate_stream_push_words(stream, ptrp, context->guardband, 4, 0);
}

static void
emit_zsa_state(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   uint32_t commands[4];
   int n = context->zsa->num_commands;

   assert(n <= (int)ARRAY_SIZE(commands));
   memcpy(commands, context->zsa->commands, n * sizeof(commands[0]));

   /*
    * Which surface holds depth belongs to the framebuffer, not to the depth
    * state object, so it has to be mixed in here. Left at zero, Z_SURF_PTR
    * names surface 0 - the colour buffer - and the depth test then reads and
    * writes depth over the colours. That is what turned every lit scene into
    * alternating columns of right and wrong pixels.
    */
   if (context->framebuffer.zs_index >= 0) {
      uint32_t surf = TGR3D_QR_Z_TEST_Z_SURF_PTR(context->framebuffer.zs_index);
      commands[1] |= surf;
      if (n > 2)
         commands[3] |= surf;
   } else {
      /* no depth buffer bound: nothing to test against or write to */
      uint32_t off = ~(TGR3D_QR_Z_TEST_Z_ENABLE__MASK |
                       TGR3D_QR_Z_TEST_QRAST_FB_WRITE__MASK);
      commands[1] &= off;
      if (n > 2)
         commands[3] &= off;
   }


   grate_stream_push_words(stream, ptrp, commands, n, 0);
}

static void
emit_vs_uniforms(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   struct pipe_constant_buffer *constbuf = &context->constant_buffer[MESA_SHADER_VERTEX];
   int len;

   if (constbuf->user_buffer != NULL) {
      assert(constbuf->buffer_size % sizeof(uint32_t) == 0);

      len = constbuf->buffer_size / 4;
      assert(len < 256 * 4);

      GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_imm(REG_TGR3D_VPE_CONST_OFFSET, 0));
      GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_nonincr(REG_TGR3D_VPE_CONST_DATA, len));
      grate_stream_push_words(stream, ptrp, constbuf->user_buffer, len, 0);
   }
}

/*
 * Fragment uniforms live in ALU register file slots 32..63, one scalar each,
 * and are uploaded as fp20 through REG_TGR3D_ALU_GLOBALS.
 */
static void
emit_fs_uniforms(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   struct pipe_constant_buffer *constbuf =
      &context->constant_buffer[MESA_SHADER_FRAGMENT];
   uint32_t values[GRATE_FP_NUM_UNIFORMS];
   unsigned num;

   if (constbuf->user_buffer == NULL)
      return;

   num = MIN2(constbuf->buffer_size / sizeof(float), GRATE_FP_NUM_UNIFORMS);
   if (num == 0)
      return;

   const float *src = constbuf->user_buffer;
   for (unsigned i = 0; i < num; ++i)
      values[i] = grate_fp20_from_float(src[i]);

   GRATE_PUSHBUF_WORD(*ptrp, host1x_opcode_incr(REG_TGR3D_ALU_GLOBALS(0), num));
   grate_stream_push_words(stream, ptrp, values, num, 0);
}

static void
emit_shader(struct grate_stream *stream, uint32_t **ptrp, struct grate_shader_blob *blob)
{
   grate_stream_push_words(stream, ptrp, blob->commands, blob->num_commands, 0);
}

static void
emit_program(struct grate_context *context, uint32_t **ptrp)
{
   struct grate_stream *stream = &context->gr3d->stream;
   uint32_t cull_face_linker_setup;

   emit_shader(stream, ptrp, &context->vshader->blob);
   emit_shader(stream, ptrp, &context->fshader->blob);

   cull_face_linker_setup = TGR3D_SU_PARAM_SUBPIX_XOFF(0x38) |
                            TGR3D_SU_PARAM_SUBPIX_YOFF(0x38) |
                            TGR3D_SU_PARAM_TRANSPOSE_XY(TGR3D_STATE_DISABLED) |
                            TGR3D_SU_PARAM_CLIP_ENABLE(TGR3D_STATE_ENABLED);

   /* depends on cull-face */
   cull_face_linker_setup |= context->rast->cull_face[context->y_invert];

   /* depends on linking */
   struct grate_fp_info *info = &context->fshader->info;
   cull_face_linker_setup |= TGR3D_SU_PARAM_LAST_INST(0 < info->num_inputs ?
                                                      (info->num_inputs - 1) : 0);

   uint32_t linker_insts[3 + info->num_inputs * 2];
   linker_insts[0] = host1x_opcode_incr(REG_TGR3D_SU_PARAM, 1);
   linker_insts[1] = cull_face_linker_setup;
   linker_insts[2] = host1x_opcode_incr(REG_TGR3D_SU_INST_EVEN(0), info->num_inputs * 2);

   for (int i = 0; i < info->num_inputs; ++i) {
      linker_insts[3 + i * 2] = info->inputs[i].src;
      linker_insts[3 + i * 2 + 1] = info->inputs[i].dst;
   }

   if (context->rast->base.flatshade && info->color_input >= 0)
      linker_insts[3 + info->color_input * 2 + 1] |= 0xf << 16;

   grate_stream_push_words(stream, ptrp, linker_insts, ARRAY_SIZE(linker_insts), 0);
}

static void
emit_rasterizer_state(struct grate_context *context, uint32_t **ptrp)
{
   if (!(context->dirty & GRATE_DIRTY_VPM))
      return;

   struct grate_rasterizer_state *rast = context->rast;
   if (!rast)
      return;

   uint32_t *ptr = *ptrp;

   GRATE_PUSHBUF_WORD(ptr, host1x_opcode_incr(REG_TGR3D_SU_PARAM, 1));
   GRATE_PUSHBUF_WORD(ptr, rast->cull_face[context->y_invert]);

   *ptrp = ptr;
}

void
grate_emit_state(struct grate_context *context, uint32_t **ptrp)
{
   emit_render_targets(context, ptrp);
   emit_viewport(context, ptrp);
   emit_guardband(context, ptrp);
   emit_scissor(context, ptrp);
   emit_rasterizer_state(context, ptrp);
   emit_zsa_state(context, ptrp);
   emit_attribs(context, ptrp);
   emit_vs_uniforms(context, ptrp);
   emit_fs_uniforms(context, ptrp);
   emit_textures(context, ptrp);
   emit_program(context, ptrp);
}

void
grate_context_vbo_init(struct pipe_context *pcontext)
{
   grate_trace();
   pcontext->create_vertex_elements_state = grate_create_vertex_state;
   pcontext->bind_vertex_elements_state = grate_bind_vertex_state;
   pcontext->delete_vertex_elements_state = grate_delete_vertex_state;
}
