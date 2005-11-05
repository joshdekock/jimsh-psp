#ifndef __pspgl_internal_h__
#define __pspgl_internal_h__

#define GL_GLEXT_PROTOTYPES

#include <GL/gl.h>
#include <GLES/egl.h>

#include "guconsts.h"

#include "pspgl_dlist.h"
#include "pspgl_hash.h"
#include "pspgl_misc.h"


#define NUM_CMDLISTS	8

struct pspgl_vertex_array {
	GLenum enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	GLboolean native;	/* size and type match hardware */
	const GLvoid *ptr;

	struct pspgl_bufferobj *buffer;
};


struct pspgl_shared_context {
	int refcount;
	struct hashtable texture_objects;
	struct hashtable display_lists;
	struct hashtable buffers;
};

struct pspgl_matrix {
	GLfloat mat[16];
};

struct pspgl_matrix_stack {
	struct pspgl_matrix *stack;
	unsigned limit;
	unsigned depth;
	unsigned dirty;		/* hardware needs updating */
};

#define VARRAY_MAX	4	/* number of arrays */
#define MAX_ATTRIB	VARRAY_MAX

#define VA_VERTEX_BIT	(1<<0)
#define VA_NORMAL_BIT	(1<<1)
#define VA_COLOR_BIT	(1<<2)
#define VA_TEXCOORD_BIT	(1<<3)

struct vertex_format
{
	unsigned hwformat;
	unsigned vertex_size;

	unsigned arrays;		/* bitmask of arrays used by this format */

	int nattrib;
	struct attrib {
		unsigned offset;	/* offset into output vertex */
		unsigned size;		/* size of element in output vertex */

		struct pspgl_vertex_array *array; /* source array */

		void (*convert)(void *to, const void *from, const struct attrib *attr);
	} attribs[MAX_ATTRIB];
};

struct pspgl_context {
	uint32_t ge_reg [256];
	uint32_t ge_reg_touched [256/32];

	struct {
		GLenum primitive;
		unsigned long vertex_count;
		void *vbuf_adr;
		GLfloat texcoord [4];
		unsigned long color;
		GLfloat normal [3];
	} current;

	struct varray {
		struct pspgl_vertex_array vertex;
		struct pspgl_vertex_array normal;
		struct pspgl_vertex_array color;
		struct pspgl_vertex_array texcoord;

		struct locked_arrays {
			GLint first;
			GLsizei count;

			struct vertex_format vfmt;

			struct pspgl_buffer *cached_array;
			unsigned cached_array_offset;
			GLint cached_first;
		} locked;

		struct pspgl_bufferobj *arraybuffer;
		struct pspgl_bufferobj *indexbuffer;
	} vertex_array;

	struct {
		uint32_t color;
		GLint stencil;
		unsigned short depth;
	} clear;

	struct {
		GLenum enabled;
		GLint x, y, width, height;
	} scissor_test;

	struct {
		unsigned char alpha;
		unsigned char stencil;
	} write_mask;

	struct {
		GLboolean positional[4];	/* set by glLight(N, GL_POSITION, ) */
		GLboolean spotlight[4];	/* set by glLight(N, GL_SPOT_CUTOFF, ) */
	} lights;

	struct {
		unsigned long ambient;
	} material;

	GLfloat depth_offset;

	/* cull_face = (front_cw ^ cull_front) ? ccw : cw */
	unsigned char front_cw;
	unsigned char cull_front;

	struct pspgl_matrix_stack projection_stack;
	struct pspgl_matrix_stack modelview_stack;
	struct pspgl_matrix_stack texture_stack;

	struct pspgl_matrix_stack *current_matrix_stack;
	struct pspgl_matrix *current_matrix;


	struct pspgl_shared_context *shared;

	struct pspgl_surface *read;
	struct pspgl_surface *draw;
	
	struct pspgl_dlist *dlist[NUM_CMDLISTS];
	struct pspgl_dlist *dlist_current;
	int dlist_idx;

	GLenum glerror;
	unsigned int swap_interval;
	int initialized;
	int refcount;

	/* XXX IMPROVE Do we really need to store anything below? these are hardware states, stored in ge_reg[]... */
	struct {
		GLint x, y, width, height;
	} viewport;
	struct {
		GLfloat near, far;
	} fog;
 	struct {
		struct pspgl_texobj	*bound;	/* currently bound texture */
 	} texture;

	struct {
		GLboolean	enabled;

		/* all times in microseconds */
		unsigned	queuewait;	/* time spent waiting for queues */
		unsigned	buffer_issues;	/* number of command buffers issued */
	} stats;
};


struct pspgl_surface {
	int pixfmt;
	unsigned long width;
	unsigned long height;
	unsigned long pixelperline;
	void *color_buffer [2];
	void *depth_buffer;
	int current_front;
	int displayed;

	unsigned alpha_mask, stencil_mask;

	/* timing stats */
	unsigned long long	flip_start, flip_end, prev_end;
};


/* pspgl_ge_init.c */
extern void __pspgl_ge_init (struct pspgl_context *c);


/* pspgl_vidmem.c */
extern EGLint __pspgl_eglerror;
extern struct pspgl_context *__pspgl_curctx;
#define pspgl_curctx	__pspgl_curctx

extern void* __pspgl_vidmem_alloc (unsigned long size);
extern void  __pspgl_vidmem_free (void * ptr);
extern EGLBoolean __pspgl_vidmem_setup_write_and_display_buffer (struct pspgl_surface *s);

/* glLockArraysEXT.c */
extern GLboolean __pspgl_cache_arrays(void);
extern void __pspgl_uncache_arrays(void);

/* pspgl_stats.c */

#include <time.h>
#include <psptypes.h>
#include <psprtc.h>

static inline unsigned long long now()
{
	unsigned long long ret;

	sceRtcGetCurrentTick(&ret);
	return ret;
}

unsigned __pspgl_ticks_to_us(unsigned long long ticks);


/* pspgl_varray.c */

extern unsigned __pspgl_gl_sizeof(GLenum type);
extern long __pspgl_glprim2geprim (GLenum glprim);
extern unsigned __pspgl_enabled_array_bits(void);
extern void __pspgl_ge_vertex_fmt(struct pspgl_context *ctx, struct vertex_format *vfmt);
extern GLboolean __pspgl_vertex_is_native(const struct vertex_format *vfmt);

extern int __pspgl_gen_varray(const struct vertex_format *vfmt, int first, int count, 
			 void *to, int space);


extern struct pspgl_buffer *__pspgl_varray_convert(const struct vertex_format *vfmt, 
						   int first, int count);
extern struct pspgl_buffer *__pspgl_varray_convert_indices(GLenum idxtype, const void *indices,
							   int first, int count,
							   unsigned *buf_offset,
							   unsigned *hwformat);


extern void __pspgl_varray_draw (GLenum mode, GLint first, GLsizei count);
extern void __pspgl_varray_draw_elts (GLenum mode, GLenum index_type, const GLvoid *indices, 
				 GLsizei count);
extern void __pspgl_varray_draw_range_elts(GLenum mode, GLenum idx_type, const void *indices, 
				      GLsizei count, int minidx, int maxidx);
extern void __pspgl_varray_bind_buffer(struct pspgl_vertex_array *va,
				       struct pspgl_bufferobj *buf);
extern void __pspgl_find_minmax_indices(GLenum idx_type, const void *indices, unsigned count,
					int *minidxp, int *maxidxp);

/* glTexImage2D.c */
struct pspgl_teximg;
extern void __pspgl_set_texture_image(struct pspgl_texobj *tobj, unsigned level, struct pspgl_teximg *timg);

static inline unsigned ispow2(unsigned n)
{
	return (n & (n-1)) == 0;
}


#define GLERROR(errcode)					\
do {								\
	__pspgl_log("*** GL error 0x%04x ***\n", errcode);		\
	if (__pspgl_curctx)						\
		__pspgl_curctx->glerror = errcode;			\
} while (0)


#define EGLERROR(errcode)					\
do {								\
	__pspgl_log("*** EGL error 0x%04x ***\n",	errcode);	\
	__pspgl_eglerror = errcode;					\
} while (0)


static inline GLclampf CLAMPF (GLfloat x)
{
	return (x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x);
}

static inline
unsigned long COLOR3 (const GLfloat c[3])
{
	return ((((int) (255.0 * CLAMPF(c[2]))) << 16) |
		(((int) (255.0 * CLAMPF(c[1]))) << 8) |
		 ((int) (255.0 * CLAMPF(c[0]))));
}


static inline
unsigned long COLOR4 (const GLfloat c[4])
{
	return ((((int) (255.0 * CLAMPF(c[3]))) << 24) |
		(((int) (255.0 * CLAMPF(c[2]))) << 16) |
		(((int) (255.0 * CLAMPF(c[1]))) << 8) |
		 ((int) (255.0 * CLAMPF(c[0]))));
}

extern const GLfloat __pspgl_identity[];

extern void __pspgl_context_writereg (struct pspgl_context *c, unsigned long cmd, unsigned long argi);
extern void __pspgl_context_writereg_masked (struct pspgl_context *c, unsigned long cmd, unsigned long argi, unsigned long mask);
extern void __pspgl_context_writereg_uncached (struct pspgl_context *c, unsigned long cmd, unsigned long argi);

extern void __pspgl_context_render_setup(struct pspgl_context *c, unsigned vtxfmt, 
					 const void *vertex, const void *index);
extern void __pspgl_context_render_prim(struct pspgl_context *c, unsigned prim, unsigned count, unsigned vtxfmt,
					const void *vertex, const void *index);
extern void __pspgl_context_pin_textures(struct pspgl_context *c);

#define sendCommandi(cmd,argi)		__pspgl_context_writereg(pspgl_curctx, cmd, argi)
#define sendCommandiUncached(cmd,argi)	__pspgl_context_writereg_uncached (pspgl_curctx, cmd, argi)

#define sendCommandf(cmd,argf)						\
do {									\
	union { float f; int i; } arg = { .f = argf };			\
	sendCommandi(cmd, arg.i >> 8);					\
} while (0)

static inline uint32_t getReg(reg)
{
	return pspgl_curctx->ge_reg[reg];
}


/* EGL stuff */
struct pspgl_pixconfig
{
	unsigned char red_bits, green_bits, blue_bits;
	unsigned char alpha_bits, stencil_bits;

	signed char hwformat;
};

extern const struct pspgl_pixconfig __pspgl_pixconfigs[];

/* Create a packed EGLconfig index */
#define EGLCONFIGIDX(pix,depth)	(((depth!=0) << 4) | (pix))

#define EGLCFG_PIXIDX(cfg)	((cfg) & 0xf)
#define EGLCFG_HASDEPTH(cfg)	(((cfg) & 0x10) >> 4)

#endif

