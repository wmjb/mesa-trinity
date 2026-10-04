#ifndef GRATE_COMPILER_H
#define GRATE_COMPILER_H

#include "util/list.h"

#include <stdint.h>


/*
 * The vertex constant file holds 256 vec4s (REG_TGR3D_VPE_CONST_READ_LIMIT is
 * programmed to 255). TGSI immediates are allocated from the top of it,
 * downwards, so they do not collide with the constant buffer at the bottom.
 */
#define GRATE_VP_NUM_CONSTS     256
#define GRATE_VP_MAX_IMMEDIATES 32
#define GRATE_VP_IMMEDIATE_SLOT(i) (GRATE_VP_NUM_CONSTS - 1 - (i))

struct grate_vp_shader {
   struct list_head instructions;
   uint16_t output_mask;

   float immediates[GRATE_VP_MAX_IMMEDIATES][4];
   unsigned num_immediates;

   /* temporaries the shader declared; scratch for staging extra attribute and
    * uniform fetches is handed out from just past this */
   unsigned num_temps;
};

struct grate_fp_info {
   struct {
      uint32_t src;
      uint32_t dst;
   } inputs[16];
   int num_inputs;
   int color_input;
   int max_tram_row;
};

/* TGSI immediates the shader declared, resolved to ALU operands at emit time */
#define GRATE_FP_MAX_IMMEDIATES 32

struct grate_fp_shader {
   struct list_head fp_instructions;
   struct list_head alu_instructions;
   struct list_head mfu_instructions;
   struct grate_fp_info info;

   float immediates[GRATE_FP_MAX_IMMEDIATES][4];
   unsigned num_immediates;

   /* temporary holding a TEX result, which lives in R2-R3 rather than a
    * general register; -1 when the shader has none */
   int tex_temp;

   /* set when the shader used something the translator cannot express, so the
    * program that reaches the GPU is a safe stub rather than a half translated
    * one */
   bool unsupported;

   /* number of TGSI temporaries the shader declared. Lowering an opcode the
    * hardware has no instruction for needs somewhere to put the intermediate,
    * and those scratch vec4s are handed out from just past this. */
   unsigned num_temps;
};


struct nir_shader;

void
grate_nir_to_vp(struct grate_vp_shader *vp, struct nir_shader *s);


void
grate_nir_to_fp(struct grate_fp_shader *fp, struct nir_shader *s);

void
grate_fp_finish(struct grate_fp_shader *fp);

#endif
