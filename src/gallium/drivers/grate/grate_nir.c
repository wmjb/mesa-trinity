/*
 * Bringing NIR into the shape this hardware can take.
 *
 * The vertex unit is a vec4 machine with no control flow, no integers and a
 * flat uniform file, so the job here is to flatten and lower everything it
 * cannot express and then optimise hard, because every instruction saved is
 * one that fits in a very small instruction memory.
 */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "grate_nir.h"

/* io is addressed in whole vec4 slots, one per location */
static unsigned
grate_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
lower_math_instr(struct nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   b->cursor = nir_before_instr(instr);

   if (alu->op == nir_op_fsqrt) {
      // fsqrt(x) -> x * frsq(x)
      nir_def *src = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *rsq = nir_frsq(b, src);
      nir_def *res = nir_fmul(b, src, rsq);
      nir_def_rewrite_uses(&alu->def, res);
      return true;
   }

   if (alu->op == nir_op_ffract) {
      // ffract(x) -> x - ffloor(x)
      nir_def *src = nir_ssa_for_alu_src(b, alu, 0);
      nir_def *flr = nir_ffloor(b, src);
      nir_def *res = nir_fsub(b, src, flr);
      nir_def_rewrite_uses(&alu->def, res);
      return true;
   }

   return false;
}

static void
grate_nir_optimize(nir_shader *s)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dead_cf);
   } while (progress);
}

static bool
swizzle_packed_varying_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   unsigned location = nir_intrinsic_io_semantics(intrin).location;

   /* 
    * GUARD 1: Never swizzle standard color varyings.
    * This protects non-textured items from turning the wrong color.
    */
   if (location == VARYING_SLOT_COL0 || location == VARYING_SLOT_COL1)
      return false;

   /* 
    * GUARD 2: Target the exact slot containing the packed S, T, Lighting data.
    * 
    * TODO: You need to replace VARYING_SLOT_TEX0 with whichever semantic 
    * slot the Mesa state tracker (or your varying linker) is assigning to 
    * this packed data.
    */
//   if (location != VARYING_SLOT_TEX0 && location != VARYING_SLOT_VAR0)
   if (location != VARYING_SLOT_VAR0)
      return false;

   nir_def *orig_val = intrin->src[0].ssa;
   
   /* If we found our packed vec3, apply the hardware swizzle */
   if (orig_val->num_components == 3) {
      b->cursor = nir_before_instr(instr);

      /* map[3] = { 1, 2, 0 } */
      unsigned swizzle[3] = {1, 2, 0};
      nir_def *swizzled = nir_swizzle(b, orig_val, swizzle, 3);
      
      nir_src_rewrite(&intrin->src[0], swizzled);
      return true;
   }

   return false;
}
static bool
grate_nir_swizzle_packed_varying(nir_shader *s)
{
   return nir_shader_instructions_pass(s, swizzle_packed_varying_instr,
                                       nir_metadata_block_index | nir_metadata_dominance,
                                       NULL);
}

void
grate_nir_lower_vs(nir_shader *s)
{
   /* there is no control flow in the vertex unit, so everything has to be
    * flattened away before translation */
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_returns);
   NIR_PASS(_, s, nir_inline_functions);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_deref);

   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries,
            nir_shader_get_entrypoint(s),
            nir_var_shader_in | nir_var_shader_out);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);

   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            grate_type_size, 0);

/* Insert the swizzle pass here, after IO is lowered */
//   NIR_PASS(_, s, grate_nir_swizzle_packed_varying);

/* Invert V texture coordinate to match hardware origin conventions */
//   grate_nir_invert_texcoord_v(s);

   /* uniforms stay as load_uniform: the vertex unit has one flat constant
    * file and no notion of a buffer to load from */


   grate_nir_optimize(s);

   NIR_PASS(_, s, nir_lower_bool_to_float, true);

   grate_nir_optimize(s);

   NIR_PASS(_, s, nir_convert_from_ssa, true, false);
   NIR_PASS(_, s, nir_opt_dce);

   nir_index_ssa_defs(nir_shader_get_entrypoint(s));

   if (getenv("GRATE_NIR_DUMP"))
      nir_print_shader(s, stderr);
}

void
grate_nir_lower_fs(nir_shader *s)
{
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_returns);
   NIR_PASS(_, s, nir_inline_functions);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_deref);

   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries,
            nir_shader_get_entrypoint(s),
            nir_var_shader_in | nir_var_shader_out);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);

/* Align fragment shader input driver locations to start at row 1 for Tegra TRAM */


   nir_foreach_shader_in_variable(var, s) {
//      var->data.driver_location += 1;
//      var->data.location += 1;
   }

   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            grate_type_size, 0);

static const struct nir_lower_tex_options tex_options = {
      .lower_rect = true,
   };
   NIR_PASS(_, s, nir_lower_tex, &tex_options);

   /* the ALU has no lerp, only multiply-add */
   NIR_PASS(_, s, nir_lower_flrp, 32, false);

/* Lower fsqrt and ffract into primitive operations */
NIR_PASS(_, s, nir_shader_instructions_pass, lower_math_instr,
            nir_metadata_none, NULL);

/* Lower power operations to log2/exp2 expansions */
//   NIR_PASS(_, s, nir_lower_pow);

   /* there are no booleans here, so a compare has to produce 1.0 or 0.0
    * directly rather than a bool that something later converts */
   NIR_PASS(_, s, nir_lower_bool_to_float, true);

   /* the fragment ALU is scalar, so give it scalar work */
   NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);

   grate_nir_optimize(s);

   NIR_PASS(_, s, nir_convert_from_ssa, true, false);
   NIR_PASS(_, s, nir_opt_dce);

   nir_index_ssa_defs(nir_shader_get_entrypoint(s));

   nir_foreach_shader_in_variable(var, s) {
//       var->data.driver_location += 1;
//       var->data.location += 1;
   }

   if (getenv("GRATE_NIR_DUMP"))
      nir_print_shader(s, stderr);
}

