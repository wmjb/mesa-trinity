/*
 * NIR to Tegra fragment program.
 *
 * The fragment ALU is scalar with four slots to a packet, so NIR is scalarised
 * and every SSA definition gets one scalar register. That is a better fit than
 * the vec4 granularity the TGSI path had to use, because the register file is
 * only 19 scalars deep.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"
#include "util/u_memory.h"
#include "util/u_math.h"

#include "grate_common.h"
#include "grate_compiler.h"
#include "fpir.h"

/* r16..r23 are the global file; r0..r3 carry varyings, TEX results and the
 * colour output and r4 holds 1/w, so r5..r15 are what is left to spill into. */
#define FP_NUM_GLOBALS 8
#define FP_SPILL_FIRST 5
#define FP_SPILL_COUNT 11
#define FP_MAX_SLOTS   12

struct fp_nir_ctx {
   struct grate_fp_shader *fp;
   int *ssa_slot;
   unsigned num_ssa;
   unsigned num_slots;
   bool overflow;

   /* per pipeline instruction */
   struct fp_mfu_instr *mfu;
   uint32_t constants[3];
   int num_constants;

   /* varying (tram row, component) -> the temporary holding it */
   int varying_slot[16][4];

   /*
    * Values that only exist during the instruction being built - an
    * interpolated varying, a TEX result - copied into a temporary so that a
    * later instruction can still read them. Each entry says where to read the
    * value from, which slot keeps it and where to record that slot.
    */
   struct {
      struct fp_alu_src_operand src;
      unsigned slot;
      int *commit;
   } keep[8];
   unsigned num_keep;

   /* varyings interpolated by the instruction being built are still in their
    * row register, which is what this instruction must read */
   int cur_row[16][4];

   /*
    * TEX writes its result into R2-R3, and the fetch issued by the previous
    * instruction is the only one those registers hold: a shader reading the
    * same texel a few instructions later gets whatever is there by then. The
    * result is kept in temporaries the same way an interpolated varying is.
    */
   const nir_def *tex_def;
   int *tex_slot;

   /* operations gathered for the instruction being built */
   /* slot reuse: a slot goes back on the free list once the value it holds
    * has been read for the last time, but not before the instruction being
    * built is emitted, or a pending operation would read a reused register */
   unsigned *last_use;
   unsigned free_slot[FP_NUM_GLOBALS + FP_SPILL_COUNT];
   unsigned num_free;
   unsigned dying[FP_MAX_SLOTS];
   unsigned num_dying;

   struct fp_alu_instr batch[FP_MAX_SLOTS];
   const nir_def *batch_def[FP_MAX_SLOTS];
   unsigned num_batch;
   const nir_def *cur_def;
   bool writes[64];
};

static unsigned
fp_reg_for_slot(struct fp_nir_ctx *ctx, unsigned slot)
{
   if (slot < FP_NUM_GLOBALS)
      return 16 + slot;

   slot -= FP_NUM_GLOBALS;
   if (slot < FP_SPILL_COUNT)
      return FP_SPILL_FIRST + slot;

   if (!ctx->overflow)
      fprintf(stderr, "GRATE FRAG: out of temporary registers\n");
   ctx->overflow = true;
   return 16;
}

static unsigned
fp_alloc_slot(struct fp_nir_ctx *ctx)
{
   if (ctx->num_free > 0 && !getenv("GRATE_FS_NORA"))
      return ctx->free_slot[--ctx->num_free];
   return ctx->num_slots++;
}

static unsigned
fp_slot_for_def(struct fp_nir_ctx *ctx, const nir_def *def)
{
   if (ctx->ssa_slot[def->index] < 0)
      ctx->ssa_slot[def->index] = fp_alloc_slot(ctx);
   return ctx->ssa_slot[def->index];
}

static struct fp_alu_src_operand
fp_src_zero(void)
{
   struct fp_alu_src_operand s = { .index = 31, .datatype = FP_DATATYPE_FIXED10 };
   return s;
}

static struct fp_alu_src_operand
fp_src_one(void)
{
   struct fp_alu_src_operand s = { .index = 31, .datatype = FP_DATATYPE_FIXED10,
                                   .sub_reg_select_high = 1 };
   return s;
}

/* TEX writes RGBA into R2-R3 as four fx10s */
static struct fp_alu_src_operand
fp_src_tex(unsigned comp)
{
   int o = comp < 3 ? (2 - comp) : 3;
   struct fp_alu_src_operand s = {
      .index = 2 + o / 2,
      .datatype = FP_DATATYPE_FIXED10,
      .sub_reg_select_high = (o % 2) != 0,
   };
   return s;
}

/* up to three fp20 constants ride in a packet's fourth slot as registers 28..30 */
static int
fp_constant(struct fp_nir_ctx *ctx, float v)
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

static struct fp_alu_src_operand
fp_nir_src(struct fp_nir_ctx *ctx, nir_src src, unsigned comp)
{
   nir_instr *parent = nir_def_instr(src.ssa);
   struct fp_alu_src_operand op = { 0 };

   switch (parent->type) {
   case nir_instr_type_load_const: {
      nir_load_const_instr *lc = nir_instr_as_load_const(parent);
      float v = comp < lc->def.num_components ? lc->value[comp].f32 : 0.0f;

      if (v == 0.0f)
         return fp_src_zero();
      if (v == 1.0f)
         return fp_src_one();

      int reg = fp_constant(ctx, v);
      if (reg < 0) {
         fprintf(stderr, "GRATE FRAG: out of embedded constants for %f\n", v);
         return fp_src_zero();
      }
      op.index = reg;
      op.datatype = FP_DATATYPE_FP20;
      return op;
   }

   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(parent);

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_interpolated_input: {
         /*
          * Varyings are interpolated once, up front, into temporaries. Asking
          * the MFU for the same varying twice does not give the same answer
          * the second time, so a shader that reads one component in two
          * instructions used to get two different values for it.
          */
         unsigned row = nir_intrinsic_base(intr);

         /* interpolated by the instruction being built: the copy of it does
          * not exist until this instruction ends, so read the row register */
         if (row < 16 && comp < 4 && ctx->cur_row[row][comp] >= 0) {
            op.index = ctx->cur_row[row][comp];
            return op;
         }

         if (row < 16 && comp < 4 && ctx->varying_slot[row][comp] >= 0 &&
             !getenv("GRATE_FS_NOCOPY")) {
            /* already interpolated once; read the copy that was kept */
            op.index = fp_reg_for_slot(ctx, ctx->varying_slot[row][comp]);
            op.datatype = FP_DATATYPE_FP20;
            return op;
         }

         if (!ctx->mfu) {
            ctx->mfu = CALLOC_STRUCT(fp_mfu_instr);
            list_inithead(&ctx->mfu->link);
         }

         ctx->mfu->var[comp].op = FP_VAR_OP_FP20;
         ctx->mfu->var[comp].tram_row = row;
         ctx->fp->info.max_tram_row = MAX2(ctx->fp->info.max_tram_row, row);

         /*
          * Keep a copy: asking the MFU for the same varying again in a later
          * instruction does not give the same answer, so a component read in
          * two instructions would otherwise come back as two different values.
          */
         if (row < 16 && comp < 4 && ctx->num_keep < ARRAY_SIZE(ctx->keep)) {
            struct fp_alu_src_operand from = { .index = comp };

            ctx->keep[ctx->num_keep].src = from;
            ctx->keep[ctx->num_keep].slot = fp_alloc_slot(ctx);
            ctx->keep[ctx->num_keep].commit = &ctx->varying_slot[row][comp];
            ctx->num_keep++;
            ctx->cur_row[row][comp] = comp;
         }

         op.index = comp;
         return op;
      }

      case nir_intrinsic_load_uniform: {
         unsigned base = nir_intrinsic_base(intr);
         unsigned off = nir_src_is_const(intr->src[0])
                           ? nir_src_as_uint(intr->src[0]) : 0;
         unsigned slot = (base + off) * 4 + comp;

         if (slot >= GRATE_FP_NUM_UNIFORMS) {
            fprintf(stderr, "GRATE FRAG: uniform slot %u past the %u the "
                            "hardware has\n", slot, GRATE_FP_NUM_UNIFORMS);
            return fp_src_zero();
         }

         op.index = GRATE_FP_UNIFORM_BASE + slot;
         op.datatype = FP_DATATYPE_FP20;
         return op;
      }

      default:
         break;
      }
      break;
   }

   case nir_instr_type_tex: {
      int *slot = &ctx->tex_slot[src.ssa->index * 4 + comp];

      /* fetched by the previous instruction: R2-R3 still hold it, and this is
       * the only instruction in which that is true */
      if (ctx->tex_def == src.ssa)
         return fp_src_tex(comp);

      if (*slot >= 0) {
         op.index = fp_reg_for_slot(ctx, *slot);
         op.datatype = FP_DATATYPE_FP20;
         return op;
      }

      fprintf(stderr, "GRATE FRAG: texture result read after R2-R3 was "
                      "overwritten\n");
      return fp_src_zero();
   }

   case nir_instr_type_alu: {
      nir_alu_instr *a = nir_instr_as_alu(parent);

      /* every source operand carries negate and absolute-value modifiers,
       * so these never need an instruction of their own */
      if (a->op == nir_op_fneg || a->op == nir_op_fabs) {
         struct fp_alu_src_operand s =
            fp_nir_src(ctx, a->src[0].src, a->src[0].swizzle[comp]);

         if (a->op == nir_op_fneg)
            s.negate = !s.negate;
         else
            s.absolute_value = true;
         return s;
      }

      /* vecN only gathers: component c is whatever source c names */
      if ((a->op == nir_op_vec2 || a->op == nir_op_vec3 ||
           a->op == nir_op_vec4) &&
          comp < nir_op_infos[a->op].num_inputs)
         return fp_nir_src(ctx, a->src[comp].src, a->src[comp].swizzle[0]);
      break;
   }

   default:
      break;
   }

   op.index = fp_reg_for_slot(ctx, fp_slot_for_def(ctx, src.ssa));
   op.datatype = FP_DATATYPE_FP20;
   return op;
}

static struct fp_alu_src_operand
fp_nir_alu_src(struct fp_nir_ctx *ctx, nir_alu_instr *alu, unsigned i)
{
   struct fp_alu_src_operand op =
      fp_nir_src(ctx, alu->src[i].src, alu->src[i].swizzle[0]);
   return op;
}

/*
 * The ALU computes rA*rB + rC, so every operation below is written in that
 * shape. MIN and MAX take the two products instead.
 */
enum fp_form {
   FP_FORM_MOV,       /* s0 * 1 + 0     */
   FP_FORM_MUL,       /* s0 * s1 + 0    */
   FP_FORM_ADD,       /* s0 * 1 + s1    */
   FP_FORM_MAD,       /* s0 * s1 + s2   */
   FP_FORM_SUB_REV,   /* s1 * 1 + (-s0) */
   FP_FORM_ONE_MINUS, /* (-s0) * 1 + 1  */
};

static struct fp_alu_instr_packet *
fp_new_packet(struct grate_fp_shader *fp)
{
   struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
   list_inithead(&pkt->link);
   return pkt;
}

static struct fp_instr *
fp_new_instr(void)
{
   struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
   list_inithead(&inst->link);
   return inst;
}

/*
 * Turn a list of scalar operations into ALU packets and one pipeline
 * instruction. Embedded constants ride in a packet's fourth slot, so only
 * three operations fit beside them.
 *
 * Any varyings this instruction interpolated are copied into temporaries here,
 * in the same instruction, so that later instructions can read them: asking
 * the MFU for the same varying again does not give the same answer.
 */
static void
fp_emit_packets(struct fp_nir_ctx *ctx, struct fp_alu_instr *slots, unsigned n)
{
   struct grate_fp_shader *fp = ctx->fp;

   for (unsigned i = 0; i < ctx->num_keep && n < FP_MAX_SLOTS; ++i) {
      struct fp_alu_dst_operand d = { 0 };

      d.index = fp_reg_for_slot(ctx, ctx->keep[i].slot);
      d.write_low_sub_reg = true;
      d.write_high_sub_reg = true;

      slots[n++] = (struct fp_alu_instr){
         .op = FP_ALU_OP_MAD,
         .condition = FP_CONDITION_ALWAYS,
         .dst = d,
         .src = { ctx->keep[i].src, fp_src_one(), fp_src_zero(),
                  fp_src_zero() },
      };
   }
   for (unsigned i = 0; i < ctx->num_keep; ++i)
      *ctx->keep[i].commit = ctx->keep[i].slot;

   ctx->num_keep = 0;
   ctx->tex_def = NULL;

   for (unsigned r = 0; r < 16; ++r)
      for (unsigned c = 0; c < 4; ++c)
         ctx->cur_row[r][c] = -1;

   if (n == 0)
      return;

   unsigned limit = ctx->num_constants > 0 ? 3 : 4;
   unsigned first = list_length(&fp->alu_instructions);
   unsigned packets = 0;

   for (unsigned i = 0; i < n; i += limit) {
      struct fp_alu_instr_packet *pkt = fp_new_packet(fp);
      unsigned count = MIN2(limit, n - i);

      for (unsigned k = 0; k < count; ++k)
         pkt->slots[k] = slots[i + k];

      if (ctx->num_constants > 0) {
         pkt->has_constants = true;
         memcpy(pkt->constants, ctx->constants, sizeof(pkt->constants));
      }

      list_addtail(&pkt->link, &fp->alu_instructions);
      packets++;
   }

   if (packets > 3)
      fprintf(stderr, "GRATE FRAG: %u ALU packets exceeds the 3 the scheduler "
                      "can issue\n", packets);

   struct fp_instr *inst = fp_new_instr();
   inst->alu_sched.address = first;
   inst->alu_sched.num_instructions = packets;

   if (ctx->mfu) {
      inst->mfu_sched.address = list_length(&fp->mfu_instructions);
      inst->mfu_sched.num_instructions = 1;
      list_addtail(&ctx->mfu->link, &fp->mfu_instructions);
   }

   list_addtail(&inst->link, &fp->fp_instructions);

   ctx->mfu = NULL;
   ctx->num_constants = 0;
   memset(ctx->writes, 0, sizeof(ctx->writes));

   for (unsigned i = 0; i < ctx->num_dying; ++i)
      if (ctx->num_free < ARRAY_SIZE(ctx->free_slot))
         ctx->free_slot[ctx->num_free++] = ctx->dying[i];
   ctx->num_dying = 0;
}

static void
fp_flush(struct fp_nir_ctx *ctx)
{
   if (ctx->num_batch == 0 && !ctx->mfu && ctx->num_keep == 0)
      return;

   struct fp_alu_instr slots[FP_MAX_SLOTS];
   unsigned n = ctx->num_batch;

   memcpy(slots, ctx->batch, n * sizeof(slots[0]));
   ctx->num_batch = 0;
   fp_emit_packets(ctx, slots, n);
}

/*
 * Gather one operation into the instruction being built. Several scalar
 * operations share an instruction, which is what keeps a program short: the
 * interpolated value a varying delivers depends on how many instructions the
 * program has, so one instruction per operation gives the wrong answer as soon
 * as a shader does more than a little work.
 *
 * An operation that reads what this instruction has already written has to
 * start a new one, and so does one that no longer fits beside the constants.
 */
static void
fp_gather(struct fp_nir_ctx *ctx, struct fp_alu_instr op)
{
   ctx->batch_def[ctx->num_batch] = ctx->cur_def;
   ctx->cur_def = NULL;
   ctx->batch[ctx->num_batch++] = op;
   if (op.dst.index < 64)
      ctx->writes[op.dst.index] = true;
}

/*
 * Decide whether the operation about to be translated can join the instruction
 * being built, before any of its sources are resolved. Resolving first would
 * be wrong: a source may allocate an embedded constant, and a flush after that
 * leaves the constant behind in the packet just emitted while the operation
 * that reads it lands in the next one.
 *
 * The ALU issues up to three packets per instruction, and a packet holding
 * embedded constants has three usable slots, so nine operations always fit.
 */
static void
fp_prepare(struct fp_nir_ctx *ctx, nir_src *srcs, unsigned n)
{
   if (ctx->num_batch + ctx->num_keep >= 9) {
      fp_flush(ctx);
      return;
   }

   for (unsigned i = 0; i < n; ++i) {
      nir_def *def = srcs[i].ssa;
      nir_instr *p = nir_def_instr(def);

      /* look through vecN, which only gathers */
      if (p->type == nir_instr_type_alu) {
         nir_alu_instr *a = nir_instr_as_alu(p);
         if (a->op == nir_op_fneg || a->op == nir_op_fabs) {
            fp_prepare(ctx, &a->src[0].src, 1);
            continue;
         }

         if (a->op == nir_op_vec2 || a->op == nir_op_vec3 ||
             a->op == nir_op_vec4) {
            for (unsigned k = 0; k < nir_op_infos[a->op].num_inputs; ++k)
               fp_prepare(ctx, &a->src[k].src, 1);
            continue;
         }
      }

      if (def->index >= ctx->num_ssa || ctx->ssa_slot[def->index] < 0)
         continue;

      unsigned reg = fp_reg_for_slot(ctx, ctx->ssa_slot[def->index]);
      if (reg < 64 && ctx->writes[reg]) {
         fp_flush(ctx);
         return;
      }
   }
}

static struct fp_alu_dst_operand
fp_dst_for_def(struct fp_nir_ctx *ctx, const nir_def *def, bool saturate)
{
   struct fp_alu_dst_operand d = { 0 };
   d.index = fp_reg_for_slot(ctx, fp_slot_for_def(ctx, def));
   d.write_low_sub_reg = true;
   d.write_high_sub_reg = true;
   d.saturate = saturate;
   return d;
}

/* the colour output lives in r2-r3 as four fx10s, swizzled RGBA -> BGRA */
static struct fp_alu_dst_operand
fp_dst_output(unsigned comp, bool saturate)
{
   struct fp_alu_dst_operand d = { 0 };
   int o = comp < 3 ? (2 - comp) : 3;

   d.index = 2 + o / 2;
   d.write_low_sub_reg = (o % 2) == 0;
   d.write_high_sub_reg = (o % 2) != 0;
   d.saturate = saturate;
   return d;
}

static void
fp_emit_form(struct fp_nir_ctx *ctx, nir_alu_instr *alu, enum fp_alu_op op,
             enum fp_form form, enum fp_condition cond)
{
   unsigned n = nir_op_infos[alu->op].num_inputs;
   struct fp_alu_src_operand s[3];

   for (unsigned i = 0; i < n && i < 3; ++i)
      fp_prepare(ctx, &alu->src[i].src, 1);

   ctx->cur_def = &alu->def;

   for (unsigned i = 0; i < n && i < 3; ++i)
      s[i] = fp_nir_alu_src(ctx, alu, i);


   struct fp_alu_src_operand rA, rB, rC;
   switch (form) {
   case FP_FORM_MOV: rA = s[0]; rB = fp_src_one();  rC = fp_src_zero(); break;
   case FP_FORM_MUL: rA = s[0]; rB = s[1];          rC = fp_src_zero(); break;
   case FP_FORM_ADD: rA = s[0]; rB = fp_src_one();  rC = s[1];          break;
   case FP_FORM_MAD: rA = s[0]; rB = s[1];          rC = s[2];          break;
   case FP_FORM_SUB_REV: rA = s[1]; rB = fp_src_one(); rC = s[0];
                     rC.negate = !rC.negate;                            break;
   case FP_FORM_ONE_MINUS: rA = s[0]; rA.negate = !rA.negate;
                     rB = fp_src_one(); rC = fp_src_one();              break;
   default: UNREACHABLE("bad fragment source form");
   }

   struct fp_alu_instr out = {
      .op = op,
      .condition = cond,
      .dst = fp_dst_for_def(ctx, &alu->def, alu->op == nir_op_fsat),
      /* src[3] mirrors src[2] so the rD selector stays off */
      .src = { rA, rB, rC, rC },
   };

   fp_gather(ctx, out);
}

/*
 * There is no NOT-EQUAL condition code - the hardware offers only EQUAL,
 * GEQUAL and GREATER - so compute the equality into a scratch register and
 * turn it inside out.
 */
static void
fp_emit_sne(struct fp_nir_ctx *ctx, nir_alu_instr *alu)
{
   unsigned scratch = fp_alloc_slot(ctx);

   {
         fp_prepare(ctx, &alu->src[0].src, 1);
      fp_prepare(ctx, &alu->src[1].src, 1);

      struct fp_alu_src_operand a = fp_nir_alu_src(ctx, alu, 0);
      struct fp_alu_src_operand b = fp_nir_alu_src(ctx, alu, 1);
      struct fp_alu_dst_operand d = { 0 };

      d.index = fp_reg_for_slot(ctx, scratch);
      d.write_low_sub_reg = true;
      d.write_high_sub_reg = true;

      a.negate = !a.negate;
      fp_gather(ctx, (struct fp_alu_instr){
         .op = FP_ALU_OP_MAD,
         .condition = FP_CONDITION_EQUAL,
         .dst = d,
         .src = { b, fp_src_one(), a, a },
      });
   }

   {
      struct fp_alu_src_operand s = { 0 };

      s.index = fp_reg_for_slot(ctx, scratch);
      s.datatype = FP_DATATYPE_FP20;
      s.negate = true;

      fp_gather(ctx, (struct fp_alu_instr){
         .op = FP_ALU_OP_MAD,
         .condition = FP_CONDITION_ALWAYS,
         .dst = fp_dst_for_def(ctx, &alu->def, false),
         .src = { s, fp_src_one(), fp_src_one(), fp_src_one() },
      });
   }

   if (ctx->num_dying < ARRAY_SIZE(ctx->dying))
      ctx->dying[ctx->num_dying++] = scratch;
}

/*
 * dst = cond(a) ? b : c, which the ALU has no single instruction for. Written
 * as c + mask * (b - c), with mask 1.0 or 0.0. fcsel's own condition is on a
 * value that is already 1.0 or 0.0, so it needs no compare; fcsel_gt and
 * fcsel_ge carry the compare and get one.
 */
static void
fp_emit_csel(struct fp_nir_ctx *ctx, nir_alu_instr *alu, enum fp_condition cond)
{
   for (unsigned i = 0; i < 3; ++i)
      fp_prepare(ctx, &alu->src[i].src, 1);

   struct fp_alu_src_operand a = fp_nir_alu_src(ctx, alu, 0);
   struct fp_alu_src_operand b = fp_nir_alu_src(ctx, alu, 1);
   struct fp_alu_src_operand c = fp_nir_alu_src(ctx, alu, 2);

   int mask = -1;

   if (cond != FP_CONDITION_ALWAYS) {
      unsigned slot = fp_alloc_slot(ctx);
      struct fp_alu_dst_operand d = { 0 };

      d.index = fp_reg_for_slot(ctx, slot);
      d.write_low_sub_reg = true;
      d.write_high_sub_reg = true;

      fp_gather(ctx, (struct fp_alu_instr){
         .op = FP_ALU_OP_MAD,
         .condition = cond,
         .dst = d,
         .src = { a, fp_src_one(), fp_src_zero(), fp_src_zero() },
      });

      a = (struct fp_alu_src_operand){ .index = d.index,
                                       .datatype = FP_DATATYPE_FP20 };
      mask = (int)slot;
   }

   unsigned diff = fp_alloc_slot(ctx);
   struct fp_alu_dst_operand d = { 0 };

   d.index = fp_reg_for_slot(ctx, diff);
   d.write_low_sub_reg = true;
   d.write_high_sub_reg = true;

   struct fp_alu_src_operand negc = c;
   negc.negate = !negc.negate;

   fp_gather(ctx, (struct fp_alu_instr){
      .op = FP_ALU_OP_MAD,
      .condition = FP_CONDITION_ALWAYS,
      .dst = d,
      .src = { b, fp_src_one(), negc, negc },
   });

   struct fp_alu_src_operand t = { .index = d.index,
                                   .datatype = FP_DATATYPE_FP20 };

   ctx->cur_def = &alu->def;
   fp_gather(ctx, (struct fp_alu_instr){
      .op = FP_ALU_OP_MAD,
      .condition = FP_CONDITION_ALWAYS,
      .dst = fp_dst_for_def(ctx, &alu->def, false),
      .src = { a, t, c, c },
   });

   if (mask >= 0 && ctx->num_dying < ARRAY_SIZE(ctx->dying))
      ctx->dying[ctx->num_dying++] = mask;
   if (ctx->num_dying < ARRAY_SIZE(ctx->dying))
      ctx->dying[ctx->num_dying++] = diff;
}

static void
fp_emit_alu(struct fp_nir_ctx *ctx, nir_alu_instr *alu)
{
   switch (alu->op) {
   case nir_op_mov:  fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_MOV,
                                  FP_CONDITION_ALWAYS); break;
   case nir_op_fmul: fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_MUL,
                                  FP_CONDITION_ALWAYS); break;
   case nir_op_fadd: fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_ADD,
                                  FP_CONDITION_ALWAYS); break;
   case nir_op_ffma: fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_MAD,
                                  FP_CONDITION_ALWAYS); break;
   case nir_op_fmin: fp_emit_form(ctx, alu, FP_ALU_OP_MIN, FP_FORM_ADD,
                                  FP_CONDITION_ALWAYS); break;
   case nir_op_fmax: fp_emit_form(ctx, alu, FP_ALU_OP_MAX, FP_FORM_ADD,
                                  FP_CONDITION_ALWAYS); break;
   /* (a < b) is (b - a > 0); the condition code turns it into 0 or 1 */
   case nir_op_slt:  fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                                  FP_CONDITION_GREATER); break;
   case nir_op_sge:  fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                                  FP_CONDITION_GEQUAL); break;
   case nir_op_seq:  fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_SUB_REV,
                                  FP_CONDITION_EQUAL); break;
   case nir_op_sne:  fp_emit_sne(ctx, alu); break;
   case nir_op_fcsel:    fp_emit_csel(ctx, alu, FP_CONDITION_ALWAYS); break;
   case nir_op_fcsel_gt: fp_emit_csel(ctx, alu, FP_CONDITION_GREATER); break;
   case nir_op_fcsel_ge: fp_emit_csel(ctx, alu, FP_CONDITION_GEQUAL); break;
   case nir_op_fsat: fp_emit_form(ctx, alu, FP_ALU_OP_MAD, FP_FORM_MOV,
                                  FP_CONDITION_ALWAYS); break;
   /* negation and absolute value are operand modifiers, not instructions */
   case nir_op_fneg:
   case nir_op_fabs:
      /* folded into whatever reads them */
      break;

   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      /* nothing to do: the components are read straight from their sources */
      break;
   default:
      fprintf(stderr, "GRATE FRAG NIR UNIMPLEMENTED: %s\n",
              nir_op_infos[alu->op].name);
      ctx->fp->unsupported = true;
      break;
   }
}

/* look through vecN gathering to the operation that actually produces a component */
static const nir_def *
fp_peel(nir_src src, unsigned *comp)
{
   nir_instr *p = nir_def_instr(src.ssa);

   if (p->type == nir_instr_type_alu) {
      nir_alu_instr *a = nir_instr_as_alu(p);

      if ((a->op == nir_op_vec2 || a->op == nir_op_vec3 ||
           a->op == nir_op_vec4) &&
          *comp < nir_op_infos[a->op].num_inputs) {
         unsigned c = a->src[*comp].swizzle[0];
         nir_src inner = a->src[*comp].src;
         *comp = c;
         return fp_peel(inner, comp);
      }
   }

   return src.ssa;
}

/*
 * Retarget the operation that produced this component at the output register,
 * instead of letting it write a temporary that a later instruction copies out.
 * The copy is not just wasted work: it adds an instruction, and a varying's
 * interpolated value depends on how many instructions the program has.
 */
static bool
fp_fuse_output(struct fp_nir_ctx *ctx, nir_src src, unsigned c)
{
   unsigned comp = c;
   const nir_def *def = fp_peel(src, &comp);

   if (comp != 0 || !list_is_singular(&def->uses))
      return false;

   for (unsigned i = 0; i < ctx->num_batch; ++i)
      if (ctx->batch_def[i] == def) {
         ctx->batch[i].dst = fp_dst_output(c, false);
         return true;
      }

   return false;
}

static void
fp_emit_store_output(struct fp_nir_ctx *ctx, nir_intrinsic_instr *intr)
{
   unsigned mask = nir_intrinsic_write_mask(intr);
   bool fused[4] = { false, false, false, false };

   /*
    * Retarget first, for every component, and only then emit copies for what
    * is left. Doing it component by component would flush the instruction
    * being built as soon as one component needed a copy, and every component
    * after that would have lost the operation it could have been fused into.
    */
   for (unsigned c = 0; c < 4; ++c)
      if (mask & (1u << c))
         fused[c] = fp_fuse_output(ctx, intr->src[0], c);

   for (unsigned c = 0; c < 4; ++c) {
      if (!(mask & (1u << c)) || fused[c])
         continue;

      unsigned comp = c;
      nir_src peeled = nir_src_for_ssa(
         (nir_def *)fp_peel(intr->src[0], &comp));

      fp_prepare(ctx, &peeled, 1);

      struct fp_alu_src_operand src = fp_nir_src(ctx, intr->src[0], c);

      fp_gather(ctx, (struct fp_alu_instr){
         .op = FP_ALU_OP_MAD,
         .condition = FP_CONDITION_ALWAYS,
         .dst = fp_dst_output(c, false),
         .src = { src, fp_src_one(), fp_src_zero(), fp_src_zero() },
      });
   }
}

static void
fp_emit_tex(struct fp_nir_ctx *ctx, nir_tex_instr *tex)
{
   fp_flush(ctx);

   struct grate_fp_shader *fp = ctx->fp;
   struct fp_instr *inst = fp_new_instr();
   int coord_idx = nir_tex_instr_src_index(tex, nir_tex_src_coord);

   if (coord_idx < 0) {
      fprintf(stderr, "GRATE FRAG NIR: texture with no coordinate\n");
      fp->unsupported = true;
      FREE(inst);
      return;
   }

   nir_src coord = tex->src[coord_idx].src;
   nir_instr *cp = nir_def_instr(coord.ssa);
   bool coord_is_varying = false;
   unsigned row = 0;

   if (cp->type == nir_instr_type_intrinsic) {
      nir_intrinsic_op op = nir_instr_as_intrinsic(cp)->intrinsic;
      if (op == nir_intrinsic_load_input ||
          op == nir_intrinsic_load_interpolated_input) {
         coord_is_varying = true;
         row = nir_intrinsic_base(nir_instr_as_intrinsic(cp));
      }
   }

   /*
    * TEX always reads S and T from row registers 0 and 1. A varying lands
    * there through the MFU; anything else has to be moved there by an ALU
    * packet in a preceding instruction.
    */
   if (coord_is_varying) {
      struct fp_mfu_instr *mfu = CALLOC_STRUCT(fp_mfu_instr);
      list_inithead(&mfu->link);

      for (unsigned c = 0; c < 2; ++c) {
         mfu->var[c].op = FP_VAR_OP_FP20;
         mfu->var[c].tram_row = row;
      }
      fp->info.max_tram_row = MAX2(fp->info.max_tram_row, row);

      inst->mfu_sched.address = list_length(&fp->mfu_instructions);
      inst->mfu_sched.num_instructions = 1;
      list_addtail(&mfu->link, &fp->mfu_instructions);
   } else {
      struct fp_instr *setup = fp_new_instr();
      struct fp_alu_instr_packet *pkt = fp_new_packet(fp);

      for (unsigned c = 0; c < 2; ++c) {
         struct fp_alu_src_operand src = fp_nir_src(ctx, coord, c);
         struct fp_alu_dst_operand d = {
            .index = c,                  /* row register 0 / 1 */
            .write_low_sub_reg = true,
            .write_high_sub_reg = true,
         };
         pkt->slots[c] = (struct fp_alu_instr){
            .op = FP_ALU_OP_MAD,
            .dst = d,
            .src = { src, fp_src_one(), fp_src_zero(), fp_src_zero() },
         };
      }

      if (ctx->num_constants > 0) {
         pkt->has_constants = true;
         memcpy(pkt->constants, ctx->constants, sizeof(pkt->constants));
         ctx->num_constants = 0;
      }

      setup->alu_sched.address = list_length(&fp->alu_instructions);
      setup->alu_sched.num_instructions = 1;
      list_addtail(&pkt->link, &fp->alu_instructions);
      list_addtail(&setup->link, &fp->fp_instructions);
   }

   inst->tex.enable = true;
   inst->tex.sampler = tex->sampler_index;
   inst->tex.dst_r2_r3 = true;
   inst->tex.src_r2_r3 = false;

   list_addtail(&inst->link, &fp->fp_instructions);

   /*
    * R2-R3 hold this result for the next instruction only, so copy out every
    * component the shader reads while it still can. Waiting for the first
    * read is too late: the instruction being built is flushed whenever an
    * operation depends on one already in it, and the texel is gone by then.
    */
   ctx->tex_def = &tex->def;

   unsigned read = nir_def_components_read(&tex->def);

   for (unsigned c = 0; c < 4; ++c) {
      if (!(read & (1u << c)) || ctx->num_keep >= ARRAY_SIZE(ctx->keep))
         continue;

      ctx->keep[ctx->num_keep].src = fp_src_tex(c);
      ctx->keep[ctx->num_keep].slot = fp_alloc_slot(ctx);
      ctx->keep[ctx->num_keep].commit = &ctx->tex_slot[tex->def.index * 4 + c];
      ctx->num_keep++;
   }
}

static void
fp_emit_intrinsic(struct fp_nir_ctx *ctx, nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_uniform:
   case nir_intrinsic_load_barycentric_pixel:
      /* resolved where they are used */
      return;

   case nir_intrinsic_store_output:
      fp_emit_store_output(ctx, intr);
      return;

   default:
      fprintf(stderr, "GRATE FRAG NIR UNIMPLEMENTED intrinsic: %s\n",
              nir_intrinsic_infos[intr->intrinsic].name);
      ctx->fp->unsupported = true;
      return;
   }
}

/*
 * Shared tail: never hand the GPU a program we could not translate, and set up
 * the barycentric weights the interpolators need.
 */
void
grate_fp_finish(struct grate_fp_shader *fp)
{
   /*
    * A shader we could not fully translate must not reach the GPU. A half
    * translated program is not merely wrong: gr3d hangs on one, and the
    * kernel then resets it in a loop until the machine goes down. Throw the
    * program away and store zeroes instead, so the surface is wrong but the
    * GPU survives and the log says why.
    */
   if (fp->unsupported) {
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

      struct fp_instr *inst = CALLOC_STRUCT(fp_instr);
      list_inithead(&inst->link);
      struct fp_alu_instr_packet *pkt = CALLOC_STRUCT(fp_alu_instr_packet);
      list_inithead(&pkt->link);
      for (int i = 0; i < 4; ++i) {
         pkt->slots[i] = (struct fp_alu_instr){
            .op = FP_ALU_OP_MAD,
            .condition = FP_CONDITION_ALWAYS,
            .dst = fp_dst_output(i, false),
            .src = { fp_src_zero(), fp_src_one(),
                     fp_src_zero(), fp_src_zero() },
         };
      }

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


struct fp_use_ctx {
   struct fp_nir_ctx *ctx;
   unsigned idx;
};

static bool
fp_note_use(nir_src *src, void *data)
{
   struct fp_use_ctx *u = data;

   u->ctx->last_use[src->ssa->index] = u->idx;
   return true;
}

static bool
fp_extend_use(nir_src *src, void *data)
{
   struct fp_use_ctx *u = data;
   unsigned *slot = &u->ctx->last_use[src->ssa->index];

   *slot = MAX2(*slot, u->idx);
   return true;
}

static bool
fp_release_use(nir_src *src, void *data)
{
   struct fp_use_ctx *u = data;
   struct fp_nir_ctx *ctx = u->ctx;
   unsigned i = src->ssa->index;

   if (ctx->last_use[i] != u->idx || ctx->ssa_slot[i] < 0)
      return true;

   if (ctx->num_dying < ARRAY_SIZE(ctx->dying))
      ctx->dying[ctx->num_dying++] = ctx->ssa_slot[i];
   ctx->ssa_slot[i] = -1;
   return true;
}

void
grate_nir_to_fp(struct grate_fp_shader *fp, nir_shader *s)
{
   list_inithead(&fp->fp_instructions);
   list_inithead(&fp->alu_instructions);
   list_inithead(&fp->mfu_instructions);

   fp->num_immediates = 0;
   fp->num_temps = 0;
   fp->unsupported = false;
   fp->tex_temp = -1;
   fp->info.num_inputs = 0;
   fp->info.color_input = -1;
   fp->info.max_tram_row = 1;

   /*
    * One linker entry per varying the shader reads. Only the components the
    * varying actually has are routed: grate's reference linker leaves the
    * rest NOP, and enabling all four shifts what the TRAM delivers.
    */
   nir_foreach_shader_in_variable(var, s) {
      unsigned row = var->data.driver_location;
      unsigned n = glsl_get_vector_elements(glsl_without_array(var->type));
      uint32_t dst = 0;

      for (unsigned i = 0; i < n && i < 4; ++i)
         dst |= LINK_DST(i, i, LINK_DST_FP20);

      fp->info.inputs[fp->info.num_inputs].src = LINK_SRC(1);
      fp->info.inputs[fp->info.num_inputs].dst = dst;
      fp->info.num_inputs++;

      fp->info.max_tram_row = MAX2(fp->info.max_tram_row, row);

      if (var->data.location == VARYING_SLOT_COL0)
         fp->info.color_input = row;
   }

   nir_function_impl *impl = nir_shader_get_entrypoint(s);

   struct fp_nir_ctx ctx = { 0 };
   ctx.fp = fp;
   for (unsigned r = 0; r < 16; ++r)
      for (unsigned c = 0; c < 4; ++c) {
         ctx.varying_slot[r][c] = -1;
         ctx.cur_row[r][c] = -1;
      }
   ctx.num_ssa = impl->ssa_alloc;
   ctx.ssa_slot = MALLOC(impl->ssa_alloc * sizeof(int));
   for (unsigned i = 0; i < impl->ssa_alloc; ++i)
      ctx.ssa_slot[i] = -1;
   ctx.tex_slot = MALLOC(impl->ssa_alloc * 4 * sizeof(int));
   for (unsigned i = 0; i < impl->ssa_alloc * 4; ++i)
      ctx.tex_slot[i] = -1;

   /*
    * Nineteen scalar registers is not many, so a slot has to go back on the
    * free list once the value in it has been read for the last time. Work out
    * where that is before translating anything.
    */
   ctx.last_use = CALLOC(impl->ssa_alloc, sizeof(unsigned));
   {
      unsigned idx = 0;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_foreach_src(instr, fp_note_use, &(struct fp_use_ctx){
               .ctx = &ctx, .idx = idx });
            idx++;
         }
      }

      /*
       * vecN only gathers and the modifiers are folded into whatever reads
       * them, so none of these emit anything: their sources have to stay live
       * until the operation that consumes the result, not until the
       * instruction that nominally reads them.
       */
      nir_foreach_block_reverse(block, impl) {
         nir_foreach_instr_reverse(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;

            nir_alu_instr *a = nir_instr_as_alu(instr);
            if (a->op != nir_op_vec2 && a->op != nir_op_vec3 &&
                a->op != nir_op_vec4 && a->op != nir_op_fneg &&
                a->op != nir_op_fabs)
               continue;

            nir_foreach_src(instr, fp_extend_use, &(struct fp_use_ctx){
               .ctx = &ctx, .idx = ctx.last_use[a->def.index] });
         }
      }
   }

   unsigned idx = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_alu:
            fp_emit_alu(&ctx, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            fp_emit_intrinsic(&ctx, nir_instr_as_intrinsic(instr));
            break;
         case nir_instr_type_tex:
            fp_emit_tex(&ctx, nir_instr_as_tex(instr));
            break;
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
         case nir_instr_type_jump:
            break;
         default:
            fprintf(stderr, "GRATE FRAG NIR: unhandled instruction type %d\n",
                    instr->type);
            fp->unsupported = true;
            break;
         }

         nir_foreach_src(instr, fp_release_use, &(struct fp_use_ctx){
            .ctx = &ctx, .idx = idx });
         idx++;
      }
   }

   fp_flush(&ctx);

   FREE(ctx.ssa_slot);
   FREE(ctx.tex_slot);
   FREE(ctx.last_use);

   /* store what the ALU built up in R2-R3, once, at the end */
   if (!list_is_empty(&fp->fp_instructions)) {
      struct fp_instr *last =
         list_last_entry(&fp->fp_instructions, struct fp_instr, link);

      last->dw.enable = 1;
      last->dw.index = 0;
      last->dw.stencil_write = 0;
      last->dw.src_regs = FP_DW_REGS_R2_R3;
   }

   if (ctx.overflow)
      fp->unsupported = true;

   grate_fp_finish(fp);
}
