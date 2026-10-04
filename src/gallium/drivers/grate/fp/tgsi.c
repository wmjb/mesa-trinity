#include "grate_common.h"
#include "grate_compiler.h"
#include "fpir.h"

#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_dump.h"
#include "util/u_math.h"
#include <stdlib.h>
#include <string.h>

#include "util/u_memory.h"

static struct fp_alu_src_operand
fp_alu_src_row(int index)
{
   assert(index >= 0 && index < 16);
   struct fp_alu_src_operand src = {
      .index = index
   };
   return src;
}

static struct fp_alu_src_operand
fp_alu_src_reg(int index)
{
   assert(index >= 0 && index < 8);
   struct fp_alu_src_operand src = {
      .index = 16 + index
   };
   return src;
}


/*
 * Scalar temporaries. Registers 16..23 are the global file; r0..r3 carry
 * varyings, TEX results and the colour output, and r4 holds 1/w for the
 * barycentric setup, so r5..r15 are free to spill into.
 */
#define GRATE_FP_NUM_GLOBALS 8
#define GRATE_FP_SPILL_FIRST 5
#define GRATE_FP_SPILL_COUNT 11

/*
 * Set when a shader asks for more scalars than the register file has. It is a
 * file static because fp_temp_reg() is called from places that have no handle
 * on the shader; grate_tgsi_to_fp() clears it before each translation, and
 * shader translation is not re-entrant.
 */
static bool fp_overflowed;

static unsigned
fp_temp_reg(unsigned slot)
{
   if (slot < GRATE_FP_NUM_GLOBALS)
      return 16 + slot;

   slot -= GRATE_FP_NUM_GLOBALS;
   if (slot < GRATE_FP_SPILL_COUNT)
      return GRATE_FP_SPILL_FIRST + slot;

   fprintf(stderr, "GRATE FRAG: out of temporary registers\n");
   fp_overflowed = true;
   return 16;
}

static struct fp_alu_src_operand
fp_alu_src_zero()
{
   struct fp_alu_src_operand src = {
      .index = 31,
      .datatype = FP_DATATYPE_FIXED10,
      .sub_reg_select_high = 0
   };
   return src;
}

static struct fp_alu_src_operand
fp_alu_src_one()
{
   struct fp_alu_src_operand src = {
      .index = 31,
      .datatype = FP_DATATYPE_FIXED10,
      .sub_reg_select_high = 1
   };
   return src;
}


/*
 * Emission context for one TGSI instruction. Embedded constants are pooled per
 * ALU packet: up to three fp20 values live in the packet's fourth slot and are
 * addressed as registers 28..30.
 */
struct fp_emit_ctx {
   struct grate_fp_shader *fp;
   struct fp_mfu_instr *mfu;
   uint32_t constants[3];
   int num_constants;
};

/* Returns the register index for a constant, or -1 if the pool is full. */
static int
fp_alloc_constant(struct fp_emit_ctx *ctx, float v)
{
   uint32_t enc = grate_fp20_from_float(v);

   for (int i = 0; i < ctx->num_constants; ++i)
      if (ctx->constants[i] == enc)
         return 28 + i;

   if (ctx->num_constants == 3)
      return -1;

   ctx->constants[ctx->num_constants] = enc;
   return 28 + ctx->num_constants++;
}


/*
 * TEX writes RGBA into R2-R3 as four fx10s, laid out exactly as fp_alu_dst()
 * places a colour output. Reading such a temporary back has to mirror that.
 */
static struct fp_alu_src_operand
fp_alu_src_tex_result(int comp)
{
   int o = comp < 3 ? (2 - comp) : 3;
   struct fp_alu_src_operand src = {
      .index = 2 + o / 2,
      .datatype = FP_DATATYPE_FIXED10,
      .sub_reg_select_high = (o % 2) != 0,
   };
   return src;
}

/*
 * Map a TGSI source component onto an ALU operand.
 *
 * Register file (docs/fragment-shader-isa.md, "Registers"):
 *    0..15 row registers (interpolated varyings)   28..30 embedded constants
 *   16..23 global registers                           31  lowp vec2(0, 1)
 *   24..27 ALU result registers                    32..63 uniform registers
 */
static struct fp_alu_src_operand
fp_alu_src_tgsi(struct fp_emit_ctx *ctx, const struct tgsi_src_register *src,
                int comp)
{
   struct fp_alu_src_operand op;

   switch (src->File) {
   case TGSI_FILE_INPUT:
      assert(ctx->mfu != NULL);
      ctx->mfu->var[comp].op = FP_VAR_OP_FP20;
      ctx->mfu->var[comp].tram_row = src->Index;
      ctx->fp->info.max_tram_row = MAX2(ctx->fp->info.max_tram_row, src->Index);
      op = fp_alu_src_row(comp);
      break;

   case TGSI_FILE_IMMEDIATE: {
      float v = 0.0f;
      if (src->Index < (int)ctx->fp->num_immediates)
         v = ctx->fp->immediates[src->Index][comp];
      else
         fprintf(stderr, "GRATE FRAG: immediate %d out of range\n", src->Index);

      /* 0.0 and 1.0 are free in the lowp constant register */
      if (v == 0.0f) { op = fp_alu_src_zero(); break; }
      if (v == 1.0f) { op = fp_alu_src_one(); break; }

      int reg = fp_alloc_constant(ctx, v);
      if (reg < 0) {
         fprintf(stderr, "GRATE FRAG: out of embedded constants for %f\n", v);
         op = fp_alu_src_zero();
         break;
      }
      struct fp_alu_src_operand c = { .index = reg, .datatype = FP_DATATYPE_FP20 };
      op = c;
      break;
   }

   case TGSI_FILE_CONSTANT: {
      /* uniform registers occupy 32..63, one scalar each */
      unsigned slot = src->Index * 4 + comp;
      if (slot >= GRATE_FP_NUM_UNIFORMS) {
         fprintf(stderr, "GRATE FRAG: uniform slot %u past the %u the hardware "
                         "has\n", slot, GRATE_FP_NUM_UNIFORMS);
         op = fp_alu_src_zero();
         break;
      }
      struct fp_alu_src_operand u = {
         .index = GRATE_FP_UNIFORM_BASE + slot,
         .datatype = FP_DATATYPE_FP20,
      };
      op = u;
      break;
   }

   case TGSI_FILE_TEMPORARY:
      if ((int)src->Index == ctx->fp->tex_temp) {
         op = fp_alu_src_tex_result(comp);
      } else {
         struct fp_alu_src_operand t = {
            .index = fp_temp_reg(src->Index * 4 + comp),
            .datatype = FP_DATATYPE_FP20,
         };
         op = t;
      }
      break;

   default: {
      struct fp_alu_src_operand t = {
         .index = fp_temp_reg(src->Index * 4 + comp),
         .datatype = FP_DATATYPE_FP20,
      };
      op = t;
      break;
   }
   }

   if (src->Negate)
      op.negate = !op.negate;
   if (src->Absolute)
      op.absolute_value = true;

   return op;
}

static struct fp_alu_instr
fp_alu_sMOV(struct fp_alu_dst_operand dst, struct fp_alu_src_operand src)
{
   struct fp_alu_instr ret = {
      .op = FP_ALU_OP_MAD,
      .dst = dst,
      .src = {
         src,
         fp_alu_src_one(),
         fp_alu_src_zero(),
         fp_alu_src_one()
      }
   };
   return ret;
}

static struct fp_alu_dst_operand
fp_alu_dst(const struct tgsi_dst_register *dst, int subreg, bool saturate)
{
   struct fp_alu_dst_operand ret = { 0 };

   ret.index = dst->Index;
   if (dst->File == TGSI_FILE_OUTPUT) {
      ret.index = 2; // HACK: r2+r3 to match hard-coded store shader for now

      // fixed10
      // swizzle RGBA -> BGRA
      int o = subreg < 3 ? (2 - subreg) : 3;
      ret.index += o / 2;
      ret.write_low_sub_reg = (o % 2) == 0;
      ret.write_high_sub_reg = (o % 2) != 0;
   } else {
      ret.index = fp_temp_reg(dst->Index * 4 + subreg);
      /* a temporary holds one fp20, so enable both subregister halves */
      ret.write_low_sub_reg = true;
      ret.write_high_sub_reg = true;
   }

   ret.saturate = saturate;

   return ret;
}

/*
 * The ALU computes rA*rB + rC*rD, with rD disabled (it is a 1-bit selector for
 * rB/rC, and "enable rD" scales rC by it), so every op below is expressed as
 * rA*rB + rC. MIN/MAX take min/max of the two products instead.
 */
enum fp_src_form {
   FP_FORM_MOV,   /* s0 * 1 + 0      */
   FP_FORM_MUL,   /* s0 * s1 + 0     */
   FP_FORM_ADD,   /* s0 * 1 + s1     */
   FP_FORM_MAD,   /* s0 * s1 + s2    */
   FP_FORM_SUB,   /* s0 * 1 + (-s1)  */
   FP_FORM_SUB_REV, /* s1 * 1 + (-s0) */
   FP_FORM_ONE_MINUS, /* (-s0) * 1 + 1  */
};

static void
emit_alu_cond(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst,
              enum fp_alu_op op, enum fp_src_form form, enum fp_condition cond);

static void
emit_alu(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst,
         enum fp_alu_op op, enum fp_src_form form)
{
   emit_alu_cond(fp, tinst, op, form, FP_CONDITION_ALWAYS);
}

static void
emit_alu_cond(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst,
              enum fp_alu_op op, enum fp_src_form form, enum fp_condition cond)
{
   const struct tgsi_dst_register *dst = &tinst->Dst[0].Register;
   bool saturate = tinst->Instruction.Saturate != 0;

   struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
   list_inithead(&inst->link);

   struct fp_emit_ctx ctx = { .fp = fp, .mfu = NULL, .num_constants = 0 };

   for (unsigned i = 0; i < tinst->Instruction.NumSrcRegs; ++i) {
      if (tinst->Src[i].Register.File == TGSI_FILE_INPUT) {
         ctx.mfu = CALLOC_STRUCT(fp_mfu_instr);
         list_inithead(&ctx.mfu->link);
         break;
      }
   }

   struct fp_alu_instr instrs[4];
   int num_instrs = 0;

   for (int i = 0; i < 4; ++i) {
      if ((dst->WriteMask & (1 << i)) == 0)
         continue;

      struct fp_alu_src_operand s[3];
      for (unsigned k = 0; k < tinst->Instruction.NumSrcRegs && k < 3; ++k) {
         const struct tgsi_src_register *sr = &tinst->Src[k].Register;
         const int sw[4] = { sr->SwizzleX, sr->SwizzleY, sr->SwizzleZ, sr->SwizzleW };
         s[k] = fp_alu_src_tgsi(&ctx, sr, sw[i]);
      }

      struct fp_alu_src_operand rA, rB, rC;
      switch (form) {
      case FP_FORM_MOV: rA = s[0]; rB = fp_alu_src_one();  rC = fp_alu_src_zero(); break;
      case FP_FORM_MUL: rA = s[0]; rB = s[1];              rC = fp_alu_src_zero(); break;
      case FP_FORM_ADD: rA = s[0]; rB = fp_alu_src_one();  rC = s[1];              break;
      case FP_FORM_MAD: rA = s[0]; rB = s[1];              rC = s[2];              break;
      case FP_FORM_SUB: rA = s[0]; rB = fp_alu_src_one();  rC = s[1];
                        rC.negate = !rC.negate;                                    break;
      case FP_FORM_SUB_REV: rA = s[1]; rB = fp_alu_src_one(); rC = s[0];
                        rC.negate = !rC.negate;                                    break;
      case FP_FORM_ONE_MINUS: rA = s[0]; rA.negate = !rA.negate;
                        rB = fp_alu_src_one(); rC = fp_alu_src_one();               break;
      default:          UNREACHABLE("bad fp source form");
      }

      struct fp_alu_instr a = {
         .op = op,
         .condition = cond,
         .dst = fp_alu_dst(dst, i, saturate),
         /* src[3] mirrors src[2] so the rD selector is stable and stays off */
         .src = { rA, rB, rC, rC },
      };
      instrs[num_instrs++] = a;
   }

   if (num_instrs == 0) {
      FREE(inst);
      FREE(ctx.mfu);
      return;
   }

   /* Constants occupy the fourth slot, so only three instructions fit then. */
   int per_packet = ctx.num_constants > 0 ? 3 : 4;
   int first_packet = list_length(&fp->alu_instructions);
   int num_packets = 0;

   for (int i = 0; i < num_instrs; i += per_packet) {
      struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
      list_inithead(&pkt->link);

      int n = MIN2(per_packet, num_instrs - i);
      for (int k = 0; k < n; ++k)
         pkt->slots[k] = instrs[i + k];

      if (ctx.num_constants > 0) {
         pkt->has_constants = true;
         memcpy(pkt->constants, ctx.constants, sizeof(pkt->constants));
      }

      list_addtail(&pkt->link, &fp->alu_instructions);
      num_packets++;
   }

   if (num_packets > 3)
      fprintf(stderr, "GRATE FRAG: %d ALU packets exceeds the 3 the scheduler "
                      "can issue\n", num_packets);

   inst->alu_sched.num_instructions = num_packets;
   inst->alu_sched.address = first_packet;

   if (ctx.mfu != NULL) {
      inst->mfu_sched.num_instructions = 1;
      inst->mfu_sched.address = list_length(&fp->mfu_instructions);
      list_addtail(&ctx.mfu->link, &fp->mfu_instructions);
   }

   if (dst->File == TGSI_FILE_OUTPUT) {
      inst->dw.enable = 1;
      inst->dw.index = dst->Index;
      inst->dw.stencil_write = 0;
      inst->dw.src_regs = FP_DW_REGS_R2_R3; // hard-coded for now
   }

   list_addtail(&inst->link, &fp->fp_instructions);
}


/*
 * TEX takes its coordinates from row registers R0/R1, which is where the MFU
 * interpolates the coordinate varying, and writes RGBA to R2-R3 - the same
 * registers the DW stage stores. This mirrors grate's reference shader:
 *
 *    MFU: ipl: t0.fp20, t0.fp20, NOP, NOP
 *    TEX: tex r2, r3, tex0, r0, r1, r2
 *    DW:  store rt1, r2, r3
 */
static void
emit_tex(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst)
{
   const struct tgsi_dst_register *dst = &tinst->Dst[0].Register;
   const struct tgsi_src_register *coord = &tinst->Src[0].Register;

   struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
   list_inithead(&inst->link);

   /*
    * TEX always reads S/T from row registers 0 and 1. A varying lands there
    * via the MFU; anything else has to be moved there by an ALU packet in a
    * preceding pipeline instruction.
    */
   if (coord->File != TGSI_FILE_INPUT) {
      struct fp_instr *setup = CALLOC_STRUCT(fp_instr);
      list_inithead(&setup->link);

      struct fp_emit_ctx sctx = { .fp = fp, .mfu = NULL, .num_constants = 0 };
      const int sw[4] = { coord->SwizzleX, coord->SwizzleY,
                          coord->SwizzleZ, coord->SwizzleW };

      struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
      list_inithead(&pkt->link);

      for (int c = 0; c < 2; ++c) {
         struct fp_alu_src_operand src = fp_alu_src_tgsi(&sctx, coord, sw[c]);
         struct fp_alu_dst_operand d = {
            .index = c,                 /* row register 0 / 1 */
            .write_low_sub_reg = true,
            .write_high_sub_reg = true,
         };
         struct fp_alu_instr a = {
            .op = FP_ALU_OP_MAD,
            .dst = d,
            .src = { src, fp_alu_src_one(), fp_alu_src_zero(), fp_alu_src_zero() },
         };
         pkt->slots[c] = a;
      }

      if (sctx.num_constants > 0) {
         pkt->has_constants = true;
         memcpy(pkt->constants, sctx.constants, sizeof(pkt->constants));
      }

      setup->alu_sched.address = list_length(&fp->alu_instructions);
      setup->alu_sched.num_instructions = 1;
      list_addtail(&pkt->link, &fp->alu_instructions);
      list_addtail(&setup->link, &fp->fp_instructions);
   }

   if (coord->File == TGSI_FILE_INPUT) {
      struct fp_mfu_instr *mfu = CALLOC_STRUCT(fp_mfu_instr);
      list_inithead(&mfu->link);

      /* S and T come from row registers 0 and 1 */
      const int sw[4] = { coord->SwizzleX, coord->SwizzleY, coord->SwizzleZ, coord->SwizzleW };
      for (int c = 0; c < 2; ++c) {
         mfu->var[sw[c]].op = FP_VAR_OP_FP20;
         mfu->var[sw[c]].tram_row = coord->Index;
      }
      fp->info.max_tram_row = MAX2(fp->info.max_tram_row, coord->Index);

      inst->mfu_sched.address = list_length(&fp->mfu_instructions);
      inst->mfu_sched.num_instructions = 1;
      list_addtail(&mfu->link, &fp->mfu_instructions);
   }

   inst->tex.enable = true;
   inst->tex.sampler = tinst->Instruction.NumSrcRegs > 1 ?
                       tinst->Src[1].Register.Index : 0;
   inst->tex.dst_r2_r3 = true;
   inst->tex.src_r2_r3 = false;

   if (dst->File == TGSI_FILE_OUTPUT) {
      inst->dw.enable = 1;
      inst->dw.index = dst->Index;
      inst->dw.src_regs = FP_DW_REGS_R2_R3;
   } else if (dst->File == TGSI_FILE_TEMPORARY) {
      fp->tex_temp = dst->Index;
   }

   list_addtail(&inst->link, &fp->fp_instructions);
}

/*
 * Scratch vec4s live just past the temporaries the shader declared, so they go
 * through the ordinary temporary register allocator and only cost registers in
 * the shaders that actually need lowering.
 */
static struct tgsi_full_src_register
fp_scratch_src(unsigned scratch)
{
   struct tgsi_full_src_register src = { 0 };
   src.Register.File = TGSI_FILE_TEMPORARY;
   src.Register.Index = scratch;
   src.Register.SwizzleX = TGSI_SWIZZLE_X;
   src.Register.SwizzleY = TGSI_SWIZZLE_Y;
   src.Register.SwizzleZ = TGSI_SWIZZLE_Z;
   src.Register.SwizzleW = TGSI_SWIZZLE_W;
   return src;
}

static struct tgsi_full_dst_register
fp_scratch_dst(unsigned scratch, unsigned writemask)
{
   struct tgsi_full_dst_register dst = { 0 };
   dst.Register.File = TGSI_FILE_TEMPORARY;
   dst.Register.Index = scratch;
   dst.Register.WriteMask = writemask;
   return dst;
}

/*
 * SNE has no condition code of its own - the hardware offers only EQUAL,
 * GEQUAL and GREATER - so compute SEQ into a scratch and turn it inside out.
 */
static void
emit_sne(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst)
{
   unsigned scratch = fp->num_temps;
   unsigned mask = tinst->Dst[0].Register.WriteMask;

   struct tgsi_full_instruction seq = *tinst;
   seq.Dst[0] = fp_scratch_dst(scratch, mask);
   emit_alu_cond(fp, &seq, FP_ALU_OP_MAD, FP_FORM_SUB_REV, FP_CONDITION_EQUAL);

   struct tgsi_full_instruction inv = *tinst;
   inv.Instruction.NumSrcRegs = 1;
   inv.Src[0] = fp_scratch_src(scratch);
   emit_alu(fp, &inv, FP_ALU_OP_MAD, FP_FORM_ONE_MINUS);
}

/*
 * LRP is s2 + s0 * (s1 - s2). The ALU computes one product plus an addend, so
 * the difference has to be worked out first.
 */
static void
emit_lrp(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst)
{
   unsigned scratch = fp->num_temps;
   unsigned mask = tinst->Dst[0].Register.WriteMask;

   struct tgsi_full_instruction sub = *tinst;
   sub.Instruction.NumSrcRegs = 2;
   sub.Src[0] = tinst->Src[1];
   sub.Src[1] = tinst->Src[2];
   sub.Dst[0] = fp_scratch_dst(scratch, mask);
   emit_alu(fp, &sub, FP_ALU_OP_MAD, FP_FORM_SUB);

   struct tgsi_full_instruction mad = *tinst;
   mad.Instruction.NumSrcRegs = 3;
   mad.Src[0] = tinst->Src[0];
   mad.Src[1] = fp_scratch_src(scratch);
   mad.Src[2] = tinst->Src[2];
   emit_alu(fp, &mad, FP_ALU_OP_MAD, FP_FORM_MAD);
}

/*
 * RCP, FRC and the rest of the transcendentals have no ALU opcode: they live in
 * the MFU's special function unit, and the SFU's answer reaches the register
 * file only by being multiplied into one of r0..r3. So an SFU op becomes an
 * ordinary MOV that stages the operand into a scratch temporary - which also
 * resolves varyings, uniforms and immediates for free - followed by one
 * instruction per component whose MFU computes the function into r0 and whose
 * ALU copies r0 where it belongs. r0 is safe to borrow: every instruction that
 * wants a varying re-interpolates it into the row registers itself.
 */
static void
emit_sfu(struct grate_fp_shader *fp, enum fp_sfu_op op,
         const struct tgsi_full_instruction *tinst,
         bool scalar_src, unsigned stage, unsigned dst_scratch, bool to_scratch)
{
   const struct tgsi_dst_register *dst = &tinst->Dst[0].Register;
   bool saturate = tinst->Instruction.Saturate != 0;
   unsigned mask = dst->WriteMask;

   struct tgsi_full_instruction mov = *tinst;
   mov.Instruction.NumSrcRegs = 1;
   mov.Instruction.Saturate = 0;
   mov.Dst[0] = fp_scratch_dst(stage, mask);
   if (scalar_src) {
      /* RCP is scalar: src.x feeds every written component */
      unsigned sx = tinst->Src[0].Register.SwizzleX;
      mov.Src[0].Register.SwizzleX = sx;
      mov.Src[0].Register.SwizzleY = sx;
      mov.Src[0].Register.SwizzleZ = sx;
      mov.Src[0].Register.SwizzleW = sx;
   }
   emit_alu(fp, &mov, FP_ALU_OP_MAD, FP_FORM_MOV);

   struct tgsi_full_dst_register scratch_dst = fp_scratch_dst(dst_scratch, mask);

   for (int i = 0; i < 4; ++i) {
      if ((mask & (1 << i)) == 0)
         continue;

      struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
      list_inithead(&inst->link);

      /*
       * The barycentric setup is folded into whichever MFU instruction comes
       * first, and an MFU instruction has only the one SFU slot, so ours must
       * never be first. If nothing has claimed that spot yet, leave an empty
       * instruction there for it and run it alongside this one.
       */
      bool need_placeholder = list_is_empty(&fp->mfu_instructions);
      if (need_placeholder) {
         struct fp_mfu_instr *ph = CALLOC_STRUCT(fp_mfu_instr);
         list_inithead(&ph->link);
         list_addtail(&ph->link, &fp->mfu_instructions);
      }

      struct fp_mfu_instr *mfu = CALLOC_STRUCT(fp_mfu_instr);
      list_inithead(&mfu->link);
      mfu->sfu.op = op;
      mfu->sfu.reg = fp_temp_reg(stage * 4 + i);
      unsigned row = i & 3;
      mfu->mul[0].dst = FP_MFU_MUL_DST_ROW_REG_0 + row;
      mfu->mul[0].src[0] = FP_MFU_MUL_SRC_SFU_RESULT;
      mfu->mul[0].src[1] = FP_MFU_MUL_SRC_CONST_1;

      inst->mfu_sched.address = list_length(&fp->mfu_instructions) -
                                (need_placeholder ? 1 : 0);
      inst->mfu_sched.num_instructions = need_placeholder ? 2 : 1;
      list_addtail(&mfu->link, &fp->mfu_instructions);

      struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
      list_inithead(&pkt->link);
      struct fp_alu_src_operand r0 = {
         .index = row,
         .datatype = FP_DATATYPE_FP20,
      };
      pkt->slots[0] = fp_alu_sMOV(
         to_scratch ? fp_alu_dst(&scratch_dst.Register, i, false)
                    : fp_alu_dst(dst, i, saturate), r0);

      inst->alu_sched.address = list_length(&fp->alu_instructions);
      inst->alu_sched.num_instructions = 1;
      list_addtail(&pkt->link, &fp->alu_instructions);

      if (!to_scratch && dst->File == TGSI_FILE_OUTPUT) {
         inst->dw.enable = 1;
         inst->dw.index = dst->Index;
         inst->dw.stencil_write = 0;
         inst->dw.src_regs = FP_DW_REGS_R2_R3;
      }

      list_addtail(&inst->link, &fp->fp_instructions);
   }
}

/* floor(x) is x - fract(x); the hardware only offers the fractional part. */
static void
emit_flr(struct grate_fp_shader *fp, const struct tgsi_full_instruction *tinst)
{
   unsigned stage = fp->num_temps;
   unsigned frc = fp->num_temps + 1;

   emit_sfu(fp, FP_SFU_OP_FRC, tinst, false, stage, frc, true);

   struct tgsi_full_instruction sub = *tinst;
   sub.Instruction.NumSrcRegs = 2;
   sub.Src[0] = tinst->Src[0];
   sub.Src[1] = fp_scratch_src(frc);
   emit_alu(fp, &sub, FP_ALU_OP_MAD, FP_FORM_SUB);
}

static void
emit_tgsi_instr(struct grate_fp_shader *fp, const struct tgsi_full_instruction *inst)
{
   if (getenv("GRATE_FP_TRACE"))
      fprintf(stderr, "GRATE FP OP: %s\n",
              tgsi_get_opcode_name(inst->Instruction.Opcode));

   switch (inst->Instruction.Opcode) {
   case TGSI_OPCODE_MOV:
      emit_alu(fp, inst, FP_ALU_OP_MAD, FP_FORM_MOV);
      break;
   case TGSI_OPCODE_MUL:
      emit_alu(fp, inst, FP_ALU_OP_MAD, FP_FORM_MUL);
      break;
   case TGSI_OPCODE_ADD:
      emit_alu(fp, inst, FP_ALU_OP_MAD, FP_FORM_ADD);
      break;
   case TGSI_OPCODE_MAD:
      emit_alu(fp, inst, FP_ALU_OP_MAD, FP_FORM_MAD);
      break;
   case TGSI_OPCODE_MIN:
      emit_alu(fp, inst, FP_ALU_OP_MIN, FP_FORM_ADD);
      break;
   case TGSI_OPCODE_MAX:
      emit_alu(fp, inst, FP_ALU_OP_MAX, FP_FORM_ADD);
      break;
   case TGSI_OPCODE_SLT:
      /* (a < b) == (b - a > 0); the condition code turns the result into 0/1 */
      emit_alu_cond(fp, inst, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                    FP_CONDITION_GREATER);
      break;
   case TGSI_OPCODE_SGE:
      emit_alu_cond(fp, inst, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                    FP_CONDITION_GEQUAL);
      break;
   case TGSI_OPCODE_CMP:
      /* CSEL is (rA < 0) ? rB : rC * rD, which is exactly TGSI CMP */
      emit_alu(fp, inst, FP_ALU_OP_CSEL, FP_FORM_MAD);
      break;
   case TGSI_OPCODE_SEQ:
      emit_alu_cond(fp, inst, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                    FP_CONDITION_EQUAL);
      break;
   case TGSI_OPCODE_SNE:
      emit_sne(fp, inst);
      break;
   case TGSI_OPCODE_LRP:
      emit_lrp(fp, inst);
      break;
   /*
    * The SFU ops below are off unless GRATE_FP_SFU is set. The special
    * function unit itself is right - sweeping the opcode field against a known
    * input reproduces RCP, RSQ, LG2, SQRT and FRC to the last bit - and a
    * shader that does nothing but one of them gives exactly the answer
    * softpipe does. What is not right yet is getting the answer back out when
    * the shader does anything else as well: as soon as an ordinary ALU
    * instruction shares the program, the MFU's result reads back as zero.
    * That is a scheduling hazard between the MFU and ALU stages, not the maths.
    * Silently wrong pixels are worse than an honest "unimplemented", so this
    * stays behind a switch until the hazard is understood.
    */
   case TGSI_OPCODE_RCP:
      if (!getenv("GRATE_FP_SFU")) goto unimplemented;
      emit_sfu(fp, FP_SFU_OP_RCP, inst, true, fp->num_temps, 0, false);
      break;
   case TGSI_OPCODE_FRC:
      if (!getenv("GRATE_FP_SFU")) goto unimplemented;
      emit_sfu(fp, FP_SFU_OP_FRC, inst, false, fp->num_temps, 0, false);
      break;
   case TGSI_OPCODE_RSQ:
      if (!getenv("GRATE_FP_SFU")) goto unimplemented;
      emit_sfu(fp, FP_SFU_OP_RSQ, inst, true, fp->num_temps, 0, false);
      break;
   case TGSI_OPCODE_SQRT:
      if (!getenv("GRATE_FP_SFU")) goto unimplemented;
      emit_sfu(fp, FP_SFU_OP_SQRT, inst, true, fp->num_temps, 0, false);
      break;
   case TGSI_OPCODE_FLR:
      if (!getenv("GRATE_FP_SFU")) goto unimplemented;
      emit_flr(fp, inst);
      break;
   case TGSI_OPCODE_TEX:
      emit_tex(fp, inst);
      break;

   default:
   unimplemented:
      fprintf(stderr, "GRATE FRAG TGSI UNIMPLEMENTED: 0x%02x (%s)\n",
              inst->Instruction.Opcode,
              tgsi_get_opcode_name(inst->Instruction.Opcode));
      fp->unsupported = true;
      break;
   }
}

#define LINK_SRC(index) ((index) << 3)
#define LINK_DST(index, comp, type) (((comp) | (type) << 2) << ((index) * 4))
#define LINK_DST_NONE      0
#define LINK_DST_FX10_LOW  1
#define LINK_DST_FX10_HIGH 2
#define LINK_DST_FP20      3

static void
emit_tgsi_input(struct grate_fp_shader *fp, const struct tgsi_full_declaration *decl)
{
   grate_trace();
   assert(decl->Range.First == decl->Range.Last);

   uint32_t src = LINK_SRC(1);
   uint32_t dst = 0;
   /*
    * Unused components stay NOP; grate's reference linker programs read
    * "LINK fp20, fp20, NOP, NOP, tram0.xyzw, export1" for a vec2 varying.
    */
   for (int i = 0; i < 4; ++i)
      if (decl->Declaration.UsageMask & (1 << i))
         dst |= LINK_DST(i, i, LINK_DST_FP20);

   fp->info.inputs[fp->info.num_inputs].src = src;
   fp->info.inputs[fp->info.num_inputs].dst = dst;

   if (decl->Declaration.Semantic == TGSI_SEMANTIC_COLOR)
      fp->info.color_input = decl->Range.First;

   fp->info.num_inputs++;
}

static void
emit_tgsi_declaration(struct grate_fp_shader *fp, const struct tgsi_full_declaration *decl)
{
   switch (decl->Declaration.File) {
   case TGSI_FILE_INPUT:
      emit_tgsi_input(fp, decl);
      break;
   case TGSI_FILE_TEMPORARY:
      fp->num_temps = MAX2(fp->num_temps, decl->Range.Last + 1);
      break;
   }
}

void
grate_tgsi_to_fp(struct grate_fp_shader *fp, struct tgsi_parse_context *tgsi)
{
   list_inithead(&fp->fp_instructions);
   list_inithead(&fp->alu_instructions);
   list_inithead(&fp->mfu_instructions);


   if (getenv("GRATE_FP_TRACE"))
      tgsi_dump(tgsi->Tokens, 0);

   fp->num_immediates = 0;
   fp->num_temps = 0;
   fp->unsupported = false;
   fp_overflowed = false;
   fp->tex_temp = -1;
   fp->info.num_inputs = 0;
   fp->info.color_input = -1;
   fp->info.max_tram_row = 1;

   while (!tgsi_parse_end_of_tokens(tgsi)) {
      tgsi_parse_token(tgsi);
      switch (tgsi->FullToken.Token.Type) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         emit_tgsi_declaration(fp, &tgsi->FullToken.FullDeclaration);
         break;

      case TGSI_TOKEN_TYPE_IMMEDIATE: {
         const struct tgsi_full_immediate *imm = &tgsi->FullToken.FullImmediate;
         if (fp->num_immediates < GRATE_FP_MAX_IMMEDIATES) {
            for (int i = 0; i < 4; ++i)
               fp->immediates[fp->num_immediates][i] = imm->u[i].Float;
            fp->num_immediates++;
         }
         break;
      }

      case TGSI_TOKEN_TYPE_INSTRUCTION:
         if (tgsi->FullToken.FullInstruction.Instruction.Opcode != TGSI_OPCODE_END)
            emit_tgsi_instr(fp, &tgsi->FullToken.FullInstruction);
         break;
      }
   }

   /*
    * A shader we could not fully translate must not reach the GPU. A half
    * translated program is not merely wrong: gr3d hangs on one, and the
    * kernel then resets it in a loop until the machine goes down. Throw the
    * program away and store zeroes instead, so the surface is wrong but the
    * GPU survives and the log says why.
    */
   if (fp->unsupported || fp_overflowed) {
      fprintf(stderr, "GRATE FRAG: shader not translatable, substituting a "
                      "stub that writes nothing\n");

      list_for_each_entry_safe(struct fp_instr, i, &fp->fp_instructions, link)
         FREE(i);
      list_for_each_entry_safe(struct fp_alu_instr_packet, p,
                               &fp->alu_instructions, link)
         FREE(p);
      list_for_each_entry_safe(struct fp_mfu_instr, m, &fp->mfu_instructions,
                               link)
         FREE(m);
      list_inithead(&fp->fp_instructions);
      list_inithead(&fp->alu_instructions);
      list_inithead(&fp->mfu_instructions);

      struct tgsi_dst_register out = { 0 };
      out.File = TGSI_FILE_OUTPUT;
      out.Index = 0;
      out.WriteMask = TGSI_WRITEMASK_XYZW;

      struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
      list_inithead(&inst->link);
      struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
      list_inithead(&pkt->link);
      for (int i = 0; i < 4; ++i)
         pkt->slots[i] = fp_alu_sMOV(fp_alu_dst(&out, i, false),
                                     fp_alu_src_zero());

      inst->alu_sched.address = 0;
      inst->alu_sched.num_instructions = 1;
      inst->dw.enable = 1;
      inst->dw.index = 0;
      inst->dw.src_regs = FP_DW_REGS_R2_R3;

      list_addtail(&pkt->link, &fp->alu_instructions);
      list_addtail(&inst->link, &fp->fp_instructions);
   }

   /*
    * Perspective interpolation needs the barycentric weights computed from
    * 1/w, and grate's reference shaders fold that into the same MFU
    * instruction that issues the interpolation:
    *
    *    MFU: sfu: rcp r4
    *         mul0: bar, sfu, bar0
    *         mul1: bar, sfu, bar1
    *         ipl: t0.fp20, t0.fp20, NOP, NOP
    *
    * A shader with no varyings has no MFU instruction at all, so give it one.
    */
   if (list_is_empty(&fp->mfu_instructions)) {
      struct fp_mfu_instr *mfu = CALLOC_STRUCT(fp_mfu_instr);
      list_inithead(&mfu->link);
      list_addtail(&mfu->link, &fp->mfu_instructions);

      list_for_each_entry(struct fp_instr, inst, &fp->fp_instructions, link) {
         inst->mfu_sched.num_instructions = 1;
         inst->mfu_sched.address = 0;
      }
   }

   /*
    * The weights are consumed by the interpolators in the very instruction
    * that computes them, so every MFU instruction that interpolates needs its
    * own copy of the setup, not just the first one. Giving it only to the
    * first left every later interpolation reading stale weights: a small error
    * in a smooth varying, and a completely wrong value in anything that feeds
    * a texture coordinate or a special function.
    *
    * An MFU instruction that interpolates nothing is left alone - that is
    * where the SFU ops do their own work, and they have no weights to compute.
    */
   list_for_each_entry(struct fp_mfu_instr, mfu, &fp->mfu_instructions, link) {
      bool interpolates = false;
      for (int i = 0; i < 4; ++i)
         if (mfu->var[i].op != FP_VAR_OP_NOP)
            interpolates = true;

      if (!interpolates && mfu->sfu.op != FP_SFU_OP_NOP)
         continue;

      mfu->sfu.op = FP_SFU_OP_RCP;
      mfu->sfu.reg = 4;
      mfu->mul[0].dst = FP_MFU_MUL_DST_BARYCENTRIC_WEIGHT;
      mfu->mul[0].src[0] = FP_MFU_MUL_SRC_SFU_RESULT;
      mfu->mul[0].src[1] = FP_MFU_MUL_SRC_BARYCENTRIC_COEF_0;
      mfu->mul[1].dst = FP_MFU_MUL_DST_BARYCENTRIC_WEIGHT;
      mfu->mul[1].src[0] = FP_MFU_MUL_SRC_SFU_RESULT;
      mfu->mul[1].src[1] = FP_MFU_MUL_SRC_BARYCENTRIC_COEF_1;
   }
}
