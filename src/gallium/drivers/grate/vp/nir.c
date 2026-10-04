/*
 * NIR to Tegra vertex program.
 *
 * The vertex unit is a vec4 VLIW with a vector slot and a scalar slot, so NIR
 * is kept vectorised and each SSA definition is given a temporary of its own.
 * The register file has 62 usable temporaries and a vertex shader that reaches
 * that is beyond this hardware anyway, so there is no reuse and no register
 * allocator - running out is reported rather than papered over.
 */
#include <stdio.h>

#include "compiler/nir/nir.h"
#include "util/u_memory.h"
#include "util/u_math.h"

#include "grate_common.h"
#include "grate_compiler.h"
#include "vpir.h"

/* the packer disables a write by naming register 63 */
#define VP_NUM_TEMPS 62

struct vp_nir_ctx {
   struct grate_vp_shader *vp;
   int *ssa_temp;          /* SSA index -> temporary, or -1 */
   unsigned num_temps;
   bool overflow;
};

static const enum vp_swz identity_swz[4] = {
   VP_SWZ_X, VP_SWZ_Y, VP_SWZ_Z, VP_SWZ_W
};

static int
vp_alloc_temp(struct vp_nir_ctx *ctx)
{
   if (ctx->num_temps >= VP_NUM_TEMPS) {
      if (!ctx->overflow)
         fprintf(stderr, "GRATE VERTEX: out of temporaries\n");
      ctx->overflow = true;
      return 0;
   }
   return ctx->num_temps++;
}

static int
vp_temp_for_def(struct vp_nir_ctx *ctx, const nir_def *def)
{
   if (ctx->ssa_temp[def->index] < 0)
      ctx->ssa_temp[def->index] = vp_alloc_temp(ctx);
   return ctx->ssa_temp[def->index];
}

/* a constant becomes an immediate, which lives at the top of the uniform file */
static int
vp_immediate(struct vp_nir_ctx *ctx, const nir_const_value *v, unsigned n)
{
   struct grate_vp_shader *vp = ctx->vp;

   for (unsigned i = 0; i < vp->num_immediates; ++i) {
      bool same = true;
      for (unsigned c = 0; c < 4; ++c) {
         float f = c < n ? v[c].f32 : 0.0f;
         if (vp->immediates[i][c] != f)
            same = false;
      }
      if (same)
         return GRATE_VP_IMMEDIATE_SLOT(i);
   }

   if (vp->num_immediates >= GRATE_VP_MAX_IMMEDIATES) {
      fprintf(stderr, "GRATE VERTEX: too many immediates\n");
      return GRATE_VP_IMMEDIATE_SLOT(0);
   }

   unsigned idx = vp->num_immediates++;
   for (unsigned c = 0; c < 4; ++c)
      vp->immediates[idx][c] = c < n ? v[c].f32 : 0.0f;

   return GRATE_VP_IMMEDIATE_SLOT(idx);
}

/*
 * Resolve one NIR source. A source is whatever instruction defined it: an
 * input becomes an attribute fetch, a uniform load a uniform fetch, a constant
 * an immediate, and anything else the temporary that instruction wrote.
 */
static struct vp_src_operand
vp_src(struct vp_nir_ctx *ctx, nir_src src, const uint8_t swizzle[4])
{
   struct vp_src_operand op = { 0 };
   nir_instr *parent = nir_def_instr(src.ssa);

   for (int i = 0; i < 4; ++i)
      op.swizzle[i] = swizzle ? (enum vp_swz)swizzle[i] : identity_swz[i];

   switch (parent->type) {
   case nir_instr_type_load_const: {
      nir_load_const_instr *lc = nir_instr_as_load_const(parent);
      op.file = VP_SRC_FILE_UNIFORM;
      op.index = vp_immediate(ctx, lc->value, lc->def.num_components);
      return op;
   }

   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(parent);

      if (intr->intrinsic == nir_intrinsic_load_input) {
         op.file = VP_SRC_FILE_ATTRIB;
         op.index = nir_intrinsic_base(intr);
         return op;
      }

      if (intr->intrinsic == nir_intrinsic_load_uniform) {
         op.file = VP_SRC_FILE_UNIFORM;
         op.index = nir_intrinsic_base(intr) +
                    (nir_src_is_const(intr->src[0])
                        ? nir_src_as_uint(intr->src[0]) : 0);
         return op;
      }
      break;
   }

   default:
      break;
   }

   op.file = VP_SRC_FILE_TEMP;
   op.index = vp_temp_for_def(ctx, src.ssa);
   return op;
}

static struct vp_dst_operand
vp_dst_temp(struct vp_nir_ctx *ctx, const nir_def *def, bool saturate)
{
   struct vp_dst_operand dst = { 0 };
   dst.file = VP_DST_FILE_TEMP;
   dst.index = vp_temp_for_def(ctx, def);
   dst.write_mask = (1u << def->num_components) - 1;
   dst.saturate = saturate;
   return dst;
}

static void
vp_push(struct grate_vp_shader *vp, struct vp_vec_instr vec,
        struct vp_scalar_instr scalar)
{
   struct vp_instr *i = CALLOC_STRUCT(vp_instr);
   list_inithead(&i->link);
   i->vec = vec;
   i->scalar = scalar;
   list_addtail(&i->link, &vp->instructions);
}

static struct vp_scalar_instr
vp_snop(void)
{
   struct vp_scalar_instr s = { 0 };
   s.op = VP_SCALAR_OP_NOP;
   s.dst.file = VP_DST_FILE_UNDEF;
   return s;
}

static struct vp_vec_instr
vp_vnop(void)
{
   struct vp_vec_instr v = { 0 };
   v.op = VP_VEC_OP_NOP;
   v.dst.file = VP_DST_FILE_UNDEF;
   return v;
}

/*
 * Only one attribute and one uniform may be fetched per instruction, so any
 * further ones are copied into temporaries first. Left unchecked the packer
 * quietly emits the wrong index and the geometry lands somewhere else.
 */
static void
vp_stage_fetches(struct vp_nir_ctx *ctx, struct vp_src_operand *src, unsigned n)
{
   int uniform = -1, attrib = -1;

   for (unsigned i = 0; i < n; ++i) {
      int *claimed;

      if (src[i].file == VP_SRC_FILE_UNIFORM)
         claimed = &uniform;
      else if (src[i].file == VP_SRC_FILE_ATTRIB)
         claimed = &attrib;
      else
         continue;

      if (*claimed < 0 || *claimed == src[i].index) {
         *claimed = src[i].index;
         continue;
      }

      int tmp = vp_alloc_temp(ctx);
      struct vp_src_operand copy = src[i];
      for (int c = 0; c < 4; ++c)
         copy.swizzle[c] = identity_swz[c];
      copy.negate = false;
      copy.absolute = false;

      struct vp_vec_instr mov = vp_vnop();
      mov.op = VP_VEC_OP_MOV;
      mov.dst.file = VP_DST_FILE_TEMP;
      mov.dst.index = tmp;
      mov.dst.write_mask = 0xf;
      mov.src[0] = copy;
      vp_push(ctx->vp, mov, vp_snop());

      src[i].file = VP_SRC_FILE_TEMP;
      src[i].index = tmp;
   }
}

static bool
vp_vec_op_for(nir_op op, enum vp_vec_op *out)
{
   switch (op) {
   case nir_op_mov:   *out = VP_VEC_OP_MOV; return true;
   case nir_op_fadd:  *out = VP_VEC_OP_ADD; return true;
   case nir_op_fmul:  *out = VP_VEC_OP_MUL; return true;
   case nir_op_ffma:  *out = VP_VEC_OP_MAD; return true;
   case nir_op_fmin:  *out = VP_VEC_OP_MIN; return true;
   case nir_op_fmax:  *out = VP_VEC_OP_MAX; return true;
   case nir_op_slt:   *out = VP_VEC_OP_SLT; return true;
   case nir_op_sge:   *out = VP_VEC_OP_SGE; return true;
   case nir_op_seq:   *out = VP_VEC_OP_SEQ; return true;
   case nir_op_sne:   *out = VP_VEC_OP_SNE; return true;
   case nir_op_ffloor:*out = VP_VEC_OP_FLR; return true;
   case nir_op_ffract:*out = VP_VEC_OP_FRC; return true;
   case nir_op_fdot3: *out = VP_VEC_OP_DP3; return true;
   case nir_op_fdot4: *out = VP_VEC_OP_DP4; return true;
   default: return false;
   }
}

static bool
vp_scalar_op_for(nir_op op, enum vp_scalar_op *out)
{
   switch (op) {
   case nir_op_frcp:  *out = VP_SCALAR_OP_RCP; return true;
   case nir_op_frsq:  *out = VP_SCALAR_OP_RSQ; return true;
   case nir_op_flog2: *out = VP_SCALAR_OP_LG2; return true;
   case nir_op_fexp2: *out = VP_SCALAR_OP_EX2; return true;
   case nir_op_fsin:  *out = VP_SCALAR_OP_SIN; return true;
   case nir_op_fcos:  *out = VP_SCALAR_OP_COS; return true;
   default: return false;
   }
}

/*
 * vecN gathers scalars into one vector. Each component may come from a
 * different place, so it becomes one move per component into the same
 * temporary, each with its own write mask.
 */
static void
vp_emit_vec(struct vp_nir_ctx *ctx, nir_alu_instr *alu, unsigned n)
{
   int dst = vp_temp_for_def(ctx, &alu->def);

   for (unsigned i = 0; i < n; ++i) {
      struct vp_src_operand src[1];
      src[0] = vp_src(ctx, alu->src[i].src, alu->src[i].swizzle);

      /* every component of the move reads the one scalar being placed */
      for (int c = 0; c < 4; ++c)
         src[0].swizzle[c] = (enum vp_swz)alu->src[i].swizzle[0];

      vp_stage_fetches(ctx, src, 1);

      struct vp_vec_instr v = vp_vnop();
      v.op = VP_VEC_OP_MOV;
      v.dst.file = VP_DST_FILE_TEMP;
      v.dst.index = dst;
      v.dst.write_mask = 1u << i;
      v.src[0] = src[0];
      vp_push(ctx->vp, v, vp_snop());
   }
}

static void
vp_emit_alu(struct vp_nir_ctx *ctx, nir_alu_instr *alu)
{
   unsigned n = nir_op_infos[alu->op].num_inputs;
   struct vp_src_operand src[3];
   enum vp_vec_op vop;
   enum vp_scalar_op sop;

   switch (alu->op) {
   case nir_op_vec2: vp_emit_vec(ctx, alu, 2); return;
   case nir_op_vec3: vp_emit_vec(ctx, alu, 3); return;
   case nir_op_vec4: vp_emit_vec(ctx, alu, 4); return;
   default: break;
   }

   if (n > 3) {
      fprintf(stderr, "GRATE VERTEX NIR: %s has %u sources\n",
              nir_op_infos[alu->op].name, n);
      return;
   }

   for (unsigned i = 0; i < n && i < 3; ++i)
      src[i] = vp_src(ctx, alu->src[i].src, alu->src[i].swizzle);

   vp_stage_fetches(ctx, src, MIN2(n, 3u));

   if (vp_scalar_op_for(alu->op, &sop)) {
      struct vp_scalar_instr s = vp_snop();
      s.op = sop;
      s.dst.file = VP_DST_FILE_TEMP;
      s.dst.index = vp_temp_for_def(ctx, &alu->def);
      s.dst.write_mask = (1u << alu->def.num_components) - 1;
      s.src = src[0];
      vp_push(ctx->vp, vp_vnop(), s);
      return;
   }

   if (!vp_vec_op_for(alu->op, &vop)) {
      fprintf(stderr, "GRATE VERTEX NIR UNIMPLEMENTED: %s\n",
              nir_op_infos[alu->op].name);
      return;
   }

   struct vp_vec_instr v = vp_vnop();
   v.op = vop;
   v.dst = vp_dst_temp(ctx, &alu->def, false);

   /*
    * Operands sit in fixed slots. ADD is the odd one out: it reads src0 and
    * src2 and leaves src1 undefined, so filling the slots in source order
    * quietly computes with a slot the instruction never reads.
    */
   if (vop == VP_VEC_OP_ADD) {
      v.src[0] = src[0];
      v.src[2] = src[1];
   } else {
      for (unsigned i = 0; i < n && i < 3; ++i)
         v.src[i] = src[i];
   }

   vp_push(ctx->vp, v, vp_snop());
}

static void
vp_emit_intrinsic(struct vp_nir_ctx *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_uniform:
      /* resolved where they are used */
      return;

   case nir_intrinsic_store_output: {
      unsigned base = nir_intrinsic_base(intr);

      struct vp_src_operand s = vp_src(ctx, intr->src[0], NULL);
      struct vp_src_operand one[3] = { s };

      vp_stage_fetches(ctx, one, 1);

      struct vp_vec_instr v = vp_vnop();
      v.op = VP_VEC_OP_MOV;
      v.dst.file = VP_DST_FILE_OUTPUT;
      v.dst.index = base;
      v.dst.write_mask = nir_intrinsic_write_mask(intr);
      v.src[0] = one[0];
      vp_push(ctx->vp, v, vp_snop());

      ctx->vp->output_mask |= 1u << base;
      return;
   }

   default:
      fprintf(stderr, "GRATE VERTEX NIR UNIMPLEMENTED intrinsic: %s\n",
              nir_intrinsic_infos[intr->intrinsic].name);
      return;
   }
}

void
grate_nir_to_vp(struct grate_vp_shader *vp, nir_shader *s)
{
   list_inithead(&vp->instructions);
   vp->output_mask = 0;
   vp->num_immediates = 0;
   vp->num_temps = 0;

   nir_function_impl *impl = nir_shader_get_entrypoint(s);

   struct vp_nir_ctx ctx = { 0 };
   ctx.vp = vp;
   ctx.ssa_temp = MALLOC(impl->ssa_alloc * sizeof(int));
   for (unsigned i = 0; i < impl->ssa_alloc; ++i)
      ctx.ssa_temp[i] = -1;

   unsigned nblocks = 0;
   nir_foreach_block(block, impl)
      nblocks++;
   if (nblocks > 1)
      fprintf(stderr, "GRATE VERTEX NIR: %u blocks; this hardware has no "
                      "control flow\n", nblocks);

   if (getenv("GRATE_VS_TRACE"))
      nir_print_shader(s, stderr);

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_alu:
            vp_emit_alu(&ctx, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            vp_emit_intrinsic(&ctx, nir_instr_as_intrinsic(instr));
            break;
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
            break;
         case nir_instr_type_jump:
            break;
         default:
            fprintf(stderr, "GRATE VERTEX NIR: unhandled instruction type %d\n",
                    instr->type);
            break;
         }
      }
   }

   FREE(ctx.ssa_temp);
}
