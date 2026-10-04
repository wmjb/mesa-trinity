/*
 * Copyright © 2026 Grate Driver Authors
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "grate_nir.h"

static bool
grate_nir_invert_texcoord_v_instr(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   unsigned location = nir_intrinsic_base(intrin);
   if (location != VARYING_SLOT_VAR0)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_def *orig_val = intrin->src[0].ssa;
   nir_def *lit     = nir_channel(b, orig_val, 0); // Lighting (.x)
   nir_def *s_coord = nir_channel(b, orig_val, 1); // S (.y)
   nir_def *t_coord = nir_channel(b, orig_val, 2); // T (.z)

   nir_def *one = nir_imm_float(b, 1.0f);
   nir_def *inv_t = nir_fsub(b, one, t_coord);
   
   /* Clamp to [0.0, 1.0] to prevent diagonal wrapping artifacts on split triangles */
   inv_t = nir_fsat(b, inv_t);

   nir_def *new_val = nir_vec3(b, s_coord, inv_t, lit);

   nir_src_rewrite(&intrin->src[0], new_val);
   return true;
}

void
grate_nir_invert_texcoord_v(nir_shader *s)
{
   nir_shader_instructions_pass(s, grate_nir_invert_texcoord_v_instr,
                                nir_metadata_block_index | nir_metadata_dominance,
                                NULL);
}
