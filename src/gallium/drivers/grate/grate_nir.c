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

   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            grate_type_size, 0);

   /* the ALU has no lerp, only multiply-add */
   NIR_PASS(_, s, nir_lower_flrp, 32, false);

   /* there are no booleans here, so a compare has to produce 1.0 or 0.0
    * directly rather than a bool that something later converts */
   NIR_PASS(_, s, nir_lower_bool_to_float, true);

   /* the fragment ALU is scalar, so give it scalar work */
   NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);

   grate_nir_optimize(s);

   NIR_PASS(_, s, nir_convert_from_ssa, true, false);
   NIR_PASS(_, s, nir_opt_dce);

   nir_index_ssa_defs(nir_shader_get_entrypoint(s));

   if (getenv("GRATE_NIR_DUMP"))
      nir_print_shader(s, stderr);
}
