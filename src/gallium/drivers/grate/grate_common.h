#ifndef GRATE_COMMON_H
#define GRATE_COMMON_H

#include "grate_screen.h"

#ifndef NDEBUG

#ifdef __GLIBC__
#define grate_msg(fmt, ...) \
   fprintf(stderr, "[%s] %s:%d/%s():\t" fmt, program_invocation_short_name, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else // __GLIBC__
#define grate_msg(fmt, ...) \
   fprintf(stderr, "%s:%d/%s():\t" fmt, __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#endif

#define VDBG_DRM(DRM, ...) do {					\
	if ((DRM)->debug_bo) { \
		grate_msg(__VA_ARGS__); \
	} \
} while (0)

#define VDBG_BO(BO, FMT, ...) do { \
	if (BO->drm->debug_bo) \
		grate_msg( \
			"BO %p size %u handle %u " \
			"flags 0x%08X refcnt %d map %p "	FMT, \
			BO, BO->size, BO->handle, \
			BO->flags, p_atomic_read(&BO->refcnt), BO->map, \
			##__VA_ARGS__); \
} while (0)

#else //NDEBUG

#define grate_msg(...) fprintf(stderr, __VA_ARGS__)
#define VDBG_DRM(DRM, FMT, ...)	do {} while (0)
#define VDBG_BO(BO, FMT, ...)	do {} while (0)

#endif // NDEBUG

#define grate_unimplemented() do { \
   if (grate_debug & GRATE_DEBUG_UNIMPLEMENTED) \
      printf("GRATE TODO: %s()\n", __func__); \
} while (0)

#define grate_trace() do { \
   if (grate_debug & GRATE_DEBUG_TRACE) \
      printf("GRATE: %s()\n", __func__); \
} while (0)


/*
 * Pixel-sequencer output window, matching libgrate. MAX_QID in
 * REG_TGR3D_GLOBAL_PIX_ATTR is derived from the same figure:
 * (GRATE_PSEQ_MAX_OUT - 1) / (alu_buffer_size * 4).
 */
#define GRATE_PSEQ_MAX_OUT   0x12c
#define GRATE_PSEQ_MIN_OUT   0x0c8
#define GRATE_ALU_BUFFER_SIZE 1

/* libgrate calls this NOT_POW2_DIMENSIONS; absent from our register header */
#define GRATE_TEXDESC_HI_NOT_POW2 0x00000040

/* fragment uniform registers: file indices 32..63, uploaded via ALU_GLOBALS */
#define GRATE_FP_UNIFORM_BASE  32
#define GRATE_FP_NUM_UNIFORMS  32

/* how long to wait for a submitted job, in nanoseconds */
#define GRATE_JOB_TIMEOUT_NS 1000000000ull

/* row stride the texture sampler assumes, in bytes */
/* IDX_DRAW_PRIM's VTX_COUNT is a 12 bit field, so this many
 * vertices go out per draw packet at most */
#define GRATE_MAX_DRAW_VERTICES 4096

#define GRATE_TEXTURE_PITCH_ALIGN 64

#endif // GRATE_COMMON_H
