#include "grate_context.h"
#include "grate_program.h"
#include "compiler/nir/nir.h"
#include "host1x01_hardware.h"

#ifndef REG_TGR3D_VPM_ATTRIB_IN
#define REG_TGR3D_VPM_ATTRIB_IN(i) (0x00000a00 + (i))
#endif

#define VPM_ATTRIB_TRAM_ROW(row)       (((uint32_t)(row) & 0x3F) << 0)
#define VPM_ATTRIB_SWZ_X(comp)        (((uint32_t)(comp) & 0x03) << 6)
#define VPM_ATTRIB_SWZ_Y(comp)        (((uint32_t)(comp) & 0x03) << 8)
#define VPM_ATTRIB_SWZ_Z(comp)        (((uint32_t)(comp) & 0x03) << 10)
#define VPM_ATTRIB_SWZ_W(comp)        (((uint32_t)(comp) & 0x03) << 12)

static const nir_variable *
grate_find_vs_output_for_fs_input(nir_shader *vs_nir, const nir_variable *fs_var)
{
   unsigned fs_loc = fs_var->data.location;
   unsigned fs_drv = fs_var->data.driver_location;

   /* 1. Try exact slot location match */
   nir_foreach_shader_out_variable(vs_var, vs_nir) {
      if (vs_var->data.location == fs_loc)
         return vs_var;
   }

   /* 2. Match VARYING_SLOT_TEX0 <-> VARYING_SLOT_VAR0 aliasing */
   if (fs_loc >= VARYING_SLOT_TEX0 && fs_loc <= VARYING_SLOT_TEX7) {
      unsigned idx = fs_loc - VARYING_SLOT_TEX0;
      nir_foreach_shader_out_variable(vs_var, vs_nir) {
         if (vs_var->data.location == (VARYING_SLOT_VAR0 + idx))
            return vs_var;
      }
   } else if (fs_loc >= VARYING_SLOT_VAR0) {
      unsigned idx = fs_loc - VARYING_SLOT_VAR0;
      nir_foreach_shader_out_variable(vs_var, vs_nir) {
         if (vs_var->data.location == (VARYING_SLOT_TEX0 + idx))
            return vs_var;
      }
   }

   /* 3. Fallback: Match by driver_location (essential for st/drawtex & internal utility shaders) */
   nir_foreach_shader_out_variable(vs_var, vs_nir) {
      if (vs_var->data.location != VARYING_SLOT_POS &&
          vs_var->data.driver_location == fs_drv) {
         return vs_var;
      }
   }

   /* 4. Single non-position output fallback */
   const nir_variable *single_out = NULL;
   int non_pos_count = 0;
   nir_foreach_shader_out_variable(vs_var, vs_nir) {
      if (vs_var->data.location != VARYING_SLOT_POS) {
         single_out = vs_var;
         non_pos_count++;
      }
   }
   if (non_pos_count == 1)
      return single_out;

   return NULL;
}

void
grate_update_vpm_state(struct grate_context *context)
{
   if (!context->vshader || !context->fshader)
      return;

   nir_shader *vs_nir = context->vshader->base.ir.nir;
   nir_shader *fs_nir = context->fshader->base.ir.nir;

   if (!vs_nir || !fs_nir)
      return;

   struct grate_vpm_state *vpm = &context->vpm;
   vpm->num_attrs = 0;

   /* Iterate over Fragment Shader input variables */
nir_foreach_shader_in_variable(fs_var, fs_nir) {
      unsigned fs_driver_loc = fs_var->data.driver_location;
unsigned fs_location = fs_var->data.location;
      unsigned fs_frac = fs_var->data.location_frac;

      const nir_variable *vs_var = grate_find_vs_output_for_fs_input(vs_nir, fs_var);

      if (!vs_var) {
         fprintf(stderr, "GRATE VPM: Unlinked FS input '%s' (slot %d)\n",
                 fs_var->name, fs_var->data.location);
         continue;
      }

      /* TRAM Row 0 is HPOS; user varyings start at driver_location //+ 1 */
      unsigned tram_row = vs_var->data.driver_location; // +1;
/* Fallback if gl_Position was assigned outside NIR IO lowering */
      if (vs_var->data.location != VARYING_SLOT_POS && tram_row == 0)
         tram_row = 1;
/*
      unsigned vs_frac = vs_var->data.location_frac;

      int comp_offset = (int)vs_frac - (int)fs_frac;

      uint8_t swz_x = CLAMP(0 + comp_offset, 0, 3);
      uint8_t swz_y = CLAMP(1 + comp_offset, 0, 3);
      uint8_t swz_z = CLAMP(2 + comp_offset, 0, 3);
      uint8_t swz_w = CLAMP(3 + comp_offset, 0, 3);

      vpm->vpm_attribs[fs_driver_loc] = VPM_ATTRIB_TRAM_ROW(tram_row) |
                                           VPM_ATTRIB_SWZ_X(swz_x)       |
                                           VPM_ATTRIB_SWZ_Y(swz_y)       |
                                           VPM_ATTRIB_SWZ_Z(swz_z)       |
                                           VPM_ATTRIB_SWZ_W(swz_w);

*/



unsigned vs_frac = vs_var->data.location_frac;
      int comp_offset = (int)vs_frac - (int)fs_frac;

      uint8_t swz_x = CLAMP(0 + comp_offset, 0, 3);
      uint8_t swz_y = CLAMP(1 + comp_offset, 0, 3);
      uint8_t swz_z = CLAMP(2 + comp_offset, 0, 3);
      uint8_t swz_w = CLAMP(3 + comp_offset, 0, 3);

      /* 
       * Architectural fix for packed generic varying slots (e.g., VARYING_SLOT_VAR0):
       * If the fragment shader expects a vec2 (e.g. texture coordinates) starting at 
       * component 0, but the vertex shader packed a scalar/lighting factor at .x 
       * and the texture coordinates at .yz, remap the VPM hardware swizzles.
       */
      if (fs_location >= VARYING_SLOT_VAR0 && fs_location <= VARYING_SLOT_VAR31) {
          unsigned fs_comps = glsl_get_components(fs_var->type);
          unsigned vs_comps = glsl_get_components(vs_var->type);

          /* If FS wants a vec2 (texcoords) from a packed vec3/vec4 slot that is offset */
          if (fs_comps == 2 && vs_comps >= 3 && vs_frac == 0) {
              swz_x = 1; // Map TRAM component 1 (.y) -> Fragment X
              swz_y = 2; // Map TRAM component 2 (.z) -> Fragment Y
          }
      }

      vpm->vpm_attribs[fs_driver_loc] = VPM_ATTRIB_TRAM_ROW(tram_row) |
                                         VPM_ATTRIB_SWZ_X(swz_x)       |
                                         VPM_ATTRIB_SWZ_Y(swz_y)       |
                                         VPM_ATTRIB_SWZ_Z(swz_z)       |
                                         VPM_ATTRIB_SWZ_W(swz_w);

      if (fs_driver_loc + 1 > vpm->num_attrs)
         vpm->num_attrs = fs_driver_loc + 1;

fprintf(stderr, "VPM MAP: FS in slot %s (loc %d, drv %d) -> VS out slot %s (loc %d, drv %d) => TRAM Row %d\n",
        fs_var->name, fs_location, fs_driver_loc,
        vs_var ? vs_var->name : "NULL", vs_var ? vs_var->data.location : -1,
        vs_var ? vs_var->data.driver_location : -1, tram_row);

   }
}

void
grate_emit_vpm_state(struct grate_context *context, uint32_t **ptr)
{
   struct grate_vpm_state *vpm = &context->vpm;

   if (vpm->num_attrs == 0)
      return;

   /* Push INCREMENT header for VPM crossbar registers */
   GRATE_PUSHBUF_WORD(*ptr, host1x_opcode_incr(REG_TGR3D_VPM_ATTRIB_IN(0),
                                              vpm->num_attrs));

   for (unsigned i = 0; i < vpm->num_attrs; i++) {
      GRATE_PUSHBUF_WORD(*ptr, vpm->vpm_attribs[i]);
   }
}
