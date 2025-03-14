#ifndef LIBGPU_H
#define LIBGPU_H

#include "types.h"
#include "PsyX/common/pgxp_defs.h"

extern	int (*GPU_printf)(const char *fmt, ...);

#define WAIT_TIME	0x800000

#define limitRange(x, l, h)	((x)=((x)<(l)?(l):(x)>(h)?(h):(x)))

#define setVector(v, _x, _y, _z) \
	(v)->vx = _x, (v)->vy = _y, (v)->vz = _z	

#define applyVector(v, _x, _y, _z, op) \
	(v)->vx op _x, (v)->vy op _y, (v)->vz op _z	

#define copyVector(v0, v1) \
	(v0)->vx = (v1)->vx, (v0)->vy = (v1)->vy, (v0)->vz = (v1)->vz 

#define addVector(v0, v1) \
	(v0)->vx += (v1)->vx,	\
	(v0)->vy += (v1)->vy,	\
	(v0)->vz += (v1)->vz	
	
#define dumpVector(str, v)	\
	GPU_printf("%s=(%d,%d,%d)\n", str, (v)->vx, (v)->vy, (v)->vz)

#define dumpMatrix(x)	\
	GPU_printf("\t%5d,%5d,%5d\n",(x)->m[0][0],(x)->m[0][1],(x)->m[0][2]),\
	GPU_printf("\t%5d,%5d,%5d\n",(x)->m[1][0],(x)->m[1][1],(x)->m[1][2]),\
	GPU_printf("\t%5d,%5d,%5d\n",(x)->m[2][0],(x)->m[2][1],(x)->m[2][2])

#define setRECT(r, _x, _y, _w, _h) \
	(r)->x = (_x),(r)->y = (_y),(r)->w = (_w),(r)->h = (_h)

/*
 *	Set Primitive Attributes
 */
#define setTPage(p,tp,abr,x,y) \
	((p)->tpage = getTPage(tp,abr,x,y))

#define setClut(p,x,y) \
	((p)->clut = getClut(x,y))
					   
/*
 * Set Primitive Colors
 */
#define setRGB0(p,_r0,_g0,_b0)						\
	(p)->r0 = _r0,(p)->g0 = _g0,(p)->b0 = _b0
	
#define setRGB1(p,_r1,_g1,_b1)						\
	(p)->r1 = _r1,(p)->g1 = _g1,(p)->b1 = _b1

#define setRGB2(p,_r2,_g2,_b2)						\
	(p)->r2 = _r2,(p)->g2 = _g2,(p)->b2 = _b2
	
#define setRGB3(p,_r3,_g3,_b3)						\
	(p)->r3 = _r3,(p)->g3 = _g3,(p)->b3 = _b3




#define setRGBP0(p,_r0,_g0,_b0,_p0)						\
	(p)->r0 = _r0,(p)->g0 = _g0,(p)->b0 = _b0,(p)->p0 = _p0

#define setRGBP1(p,_r1,_g1,_b1,_p1)						\
	(p)->r1 = _r1,(p)->g1 = _g1,(p)->b1 = _b1,(p)->p1 = _p1

#define setRGBP2(p,_r2,_g2,_b2,_p2)						\
	(p)->r2 = _r2,(p)->g2 = _g2,(p)->b2 = _b2,(p)->p2 = _p2

#define setRGBP3(p,_r3,_g3,_b3,_p3)						\
	(p)->r3 = _r3,(p)->g3 = _g3,(p)->b3 = _b3,(p)->p3 = _p3
	
/*
 * Set Primitive Screen Points
 */
#define setXY0(p,_x0,_y0)						\
	(p)->x0 = (_x0), (p)->y0 = (_y0)				\

#define setXY2(p,_x0,_y0,_x1,_y1)					\
	(p)->x0 = (_x0), (p)->y0 = (_y0),				\
	(p)->x1 = (_x1), (p)->y1 = (_y1)

#define setXY3(p,_x0,_y0,_x1,_y1,_x2,_y2)				\
	(p)->x0 = (_x0), (p)->y0 = (_y0),				\
	(p)->x1 = (_x1), (p)->y1 = (_y1),				\
	(p)->x2 = (_x2), (p)->y2 = (_y2)

#define setXY4(p,_x0,_y0,_x1,_y1,_x2,_y2,_x3,_y3) 			\
	(p)->x0 = (_x0), (p)->y0 = (_y0),				\
	(p)->x1 = (_x1), (p)->y1 = (_y1),				\
	(p)->x2 = (_x2), (p)->y2 = (_y2),				\
	(p)->x3 = (_x3), (p)->y3 = (_y3)

#define setXYWH(p,_x0,_y0,_w,_h)					\
	(p)->x0 = (_x0),      (p)->y0 = (_y0),				\
	(p)->x1 = (_x0)+(_w), (p)->y1 = (_y0),				\
	(p)->x2 = (_x0),      (p)->y2 = (_y0)+(_h),			\
	(p)->x3 = (_x0)+(_w), (p)->y3 = (_y0)+(_h)

/*
 * Set Primitive Width/Height
 */
#define setWH(p,_w,_h)	(p)->w = _w, (p)->h = _h

/*
 * Set Primitive Texture Points
 */
#define setUV0(p,_u0,_v0)						\
	(p)->u0 = (_u0), (p)->v0 = (_v0)				\
	
#define setUV3(p,_u0,_v0,_u1,_v1,_u2,_v2)				\
	(p)->u0 = (_u0), (p)->v0 = (_v0),				\
	(p)->u1 = (_u1), (p)->v1 = (_v1),				\
	(p)->u2 = (_u2), (p)->v2 = (_v2)
	
#define setUV4(p,_u0,_v0,_u1,_v1,_u2,_v2,_u3,_v3) 			\
	(p)->u0 = (_u0), (p)->v0 = (_v0),				\
	(p)->u1 = (_u1), (p)->v1 = (_v1),				\
	(p)->u2 = (_u2), (p)->v2 = (_v2),				\
	(p)->u3 = (_u3), (p)->v3 = (_v3)

#define setUVWH(p,_u0,_v0,_w,_h)					\
	(p)->u0 = (_u0),      (p)->v0 = (_v0),				\
	(p)->u1 = (_u0)+(_w), (p)->v1 = (_v0),				\
	(p)->u2 = (_u0),      (p)->v2 = (_v0)+(_h),			\
	(p)->u3 = (_u0)+(_w), (p)->v3 = (_v0)+(_h)

	
/*
 * Dump Primivie Parameters
 */
#define dumpRECT16(r)	\
	GPU_printf("(%d,%d)-(%d,%d)\n", (r)->x,(r)->y,(r)->w,(r)->h)

#define dumpWH(p)	GPU_printf("(%d,%d)\n", (p)->w,  (p)->h ) 
#define dumpXY0(p)	GPU_printf("(%d,%d)\n", (p)->x0, (p)->y0) 
#define dumpUV0(p)	GPU_printf("(%d,%d)\n", (p)->u0, (p)->v0) 

#define dumpXY2(p)							\
	GPU_printf("(%d,%d)-(%d,%d)\n",					\
	(p)->x0, (p)->y0, (p)->x1, (p)->y1)

#define dumpXY3(p)							\
	GPU_printf("(%d,%d)-(%d,%d)-(%d,%d)\n",				\
	(p)->x0, (p)->y0, (p)->x1, (p)->y1,				\
	(p)->x2, (p)->y2)

#define dumpUV3(p)							\
	GPU_printf("(%d,%d)-(%d,%d)-(%d,%d)\n",				\
	(p)->u0, (p)->v0, (p)->u1, (p)->v1,				\
	(p)->u2, (p)->v2)

#define dumpXY4(p)							\
	GPU_printf("(%d,%d)-(%d,%d)-(%d,%d)-(%d,%d)\n",			\
	(p)->x0, (p)->y0, (p)->x1, (p)->y1,				\
	(p)->x2, (p)->y2, (p)->x3, (p)->y3)

#define dumpUV4(p)							\
	GPU_printf("(%d,%d)-(%d,%d)-(%d,%d)-(%d,%d)\n",			\
	(p)->u0, (p)->v0, (p)->u1, (p)->v1,				\
	(p)->u2, (p)->v2, (p)->u3, (p)->v3)			

#define dumpRGB0(p)							\
	GPU_printf("(%3d,%3d,%3d)\n", (p)->r0, (p)->g0, (p)->b0) 	
		   
#define dumpRGB1(p)							\
	GPU_printf("(%3d,%3d,%3d)\n", (p)->r1, (p)->g1, (p)->b1)	
		   
#define dumpRGB2(p)							\
	GPU_printf("(%3d,%3d,%3d)\n", (p)->r2, (p)->g2, (p)->b2) 
		   
#define dumpRGB3(p)							\
	GPU_printf("(%3d,%3d,%3d)\n", (p)->r3, (p)->g3, (p)->b3) 	

 /*
  * Primitive Handling Macros
  */

#if USE_EXTENDED_PRIM_POINTERS

#define isendprim(p) 		((((P_TAG *)(p))->addr) == (uintptr_t)&prim_terminator)
#define nextPrim(p)  		(void *)(((P_TAG *)(p))->addr)

#define setaddr(p, _addr)	(((P_TAG *)(p))->addr = (uintptr_t)((u_int*)_addr))
#define getaddr(p)   		(uintptr_t)(((P_TAG *)(p))->addr)

#else

#define isendprim(p) 		((((P_TAG *)(p))->addr)==0xffffff)
#define nextPrim(p)  		(void *)((((P_TAG *)(p))->addr))

#define setaddr(p, _addr)	(((P_TAG *)(p))->addr = (u_int)((u_int*)_addr))
#define getaddr(p)   		(u_int)(((P_TAG *)(p))->addr)

#endif

#define setlen( p, _len) 	(((P_TAG *)(p))->len  = (u_char)(_len))
#define setcode(p, _code)	(((P_TAG *)(p))->code = (u_char)(_code))

#define getlen(p)    		(u_char)(((P_TAG *)(p))->len)
#define getcode(p)   		(u_char)(((P_TAG *)(p))->code)

#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
#define setpgxpindex(p, i)	(((P_TAG *)(p))->pgxp_index = (u_short)(i))
#define addPrim(ot, p)		setaddr(p, getaddr(ot)), setaddr(ot, p), setpgxpindex(p, PGXP_GetIndex(1))
#else
#define addPrim(ot, p)		setaddr(p, getaddr(ot)), setaddr(ot, p)
#endif

#define addPrims(ot, p0, p1)	setaddr(p1, getaddr(ot)),setaddr(ot, p0)

#define catPrim(p0, p1)		setaddr(p0, p1)

#if USE_EXTENDED_PRIM_POINTERS
#define termPrim(p)			setaddr(p, &prim_terminator)
#else
#define termPrim(p)			setaddr(p, 0xffffffff)
#endif

#define setSemiTrans(p, abe) \
	((abe)?setcode(p, getcode(p)|0x02):setcode(p, getcode(p)&~0x02))

#define setShadeTex(p, tge) \
	((tge)?setcode(p, getcode(p)|0x01):setcode(p, getcode(p)&~0x01))

#define getTPage(tp, abr, x, y) 					\
	 ((((tp)&0x3)<<7)|(((abr)&0x3)<<5)|(((y)&0x100)>>4)|(((x)&0x3ff)>>6)| \
	 (((y)&0x200)<<2))

#define getClut(x, y) \
	(((y)<<6)|(((x)>>4)&0x3f))

#define dumpTPage(tpage)						\
	GPU_printf("tpage: (%d,%d,%d,%d)\n",				\
			   ((tpage)>>7)&0x3,((tpage)>>5)&0x3,	\
			   ((tpage)<<6)&0x3c0,				\
			   (((tpage)<<4)&0x100)+(((tpage)>>2)&0x200))

#define dumpClut(clut) \
	GPU_printf("clut: (%d,%d)\n", (clut&0x3f)<<4, (clut>>6))

#define _get_mode(dfe, dtd, tpage)	\
		((0xe1000000)|((dtd)?0x0200:0)| \
		((dfe)?0x0400:0)|((tpage)&0x9ff))

#define setDrawTPage(p, dfe, dtd, tpage)	\
		setlen(p, 1),	\
		((p)->code[0] = _get_mode(dfe, dtd, tpage))

#define _get_tw(tw)	\
		(tw ? ((0xe2000000)|((((tw)->y&0xff)>>3)<<15)| \
		((((tw)->x&0xff)>>3)<<10)|(((~((tw)->h-1)&0xff)>>3)<<5)| \
		(((~((tw)->w-1)&0xff)>>3))) : 0)

#define setTexWindow(p, tw)			\
		setlen(p, 2),				\
		((p)->code[0] = _get_tw(tw)),	\
		((p)->code[1] = 0)

#define _get_len(rect)	\
		(((RECT16)->w*(rect)->h+1)/2+4)

#define setDrawLoad(pt, rect)					\
	(_get_len(RECT16) <= 16) ? (				\
		(setlen(pt, _get_len(rect))),			\
		((pt)->code[0] = 0xa0000000),			\
		((pt)->code[1] = *((u_int *)&(rect)->x)),	\
		((pt)->code[2] = *((u_int *)&(rect)->w)),	\
		((pt)->p[_get_len(rect)-4] = 0x01000000)	\
	) : ( \
		(setlen(pt,0)) \
	)

#define setDrawStp(p, pbw) 				\
		setlen(p, 2),					\
		((p)->code[0] = 0xe6000000|(pbw?0x01:0)),	\
		((p)->code[1] = 0)

#define setDrawMode(p, dfe, dtd, tpage, tw) 		\
		setlen(p, 3),					\
		((p)->code[0] = _get_mode(dfe, dtd, tpage)),	\
		((p)->code[1] = _get_tw((RECT16 *)tw))

	
/*	Primitive 	Lentgh		Code				*/
/*--------------------------------------------------------------------	*/
/*									*/
#define setPolyF3(p)	setlen(p, 4),  setcode(p, 0x20)
#define setPolyFT3(p)	setlen(p, 7),  setcode(p, 0x24)
#define setPolyG3(p)	setlen(p, 6),  setcode(p, 0x30)
#define setPolyGT3(p)	setlen(p, 9),  setcode(p, 0x34)
#define setPolyF4(p)	setlen(p, 5),  setcode(p, 0x28)
#define setPolyFT4(p)	setlen(p, 9),  setcode(p, 0x2c)
#define setPolyG4(p)	setlen(p, 8),  setcode(p, 0x38)
#define setPolyGT4(p)	setlen(p, 12), setcode(p, 0x3c)

#define setSprt8(p)	setlen(p, 3),  setcode(p, 0x74)
#define setSprt16(p)	setlen(p, 3),  setcode(p, 0x7c)
#define setSprt(p)	setlen(p, 4),  setcode(p, 0x64)

#define setTile1(p)	setlen(p, 2),  setcode(p, 0x68)
#define setTile8(p)	setlen(p, 2),  setcode(p, 0x70)
#define setTile16(p)	setlen(p, 2),  setcode(p, 0x78)
#define setTile(p)	setlen(p, 3),  setcode(p, 0x60)
#define setLineF2(p)	setlen(p, 3),  setcode(p, 0x40)
#define setLineG2(p)	setlen(p, 4),  setcode(p, 0x50)
#define setLineF3(p)	setlen(p, 5),  setcode(p, 0x48),(p)->pad = 0x55555555
#define setLineG3(p)	setlen(p, 7),  setcode(p, 0x58),(p)->pad = 0x55555555, \
			(p)->p2 = 0
#define setLineF4(p)	setlen(p, 6),  setcode(p, 0x4c),(p)->pad = 0x55555555
#define setLineG4(p)	setlen(p, 9),  setcode(p, 0x5c),(p)->pad = 0x55555555, \
			(p)->p2 = 0, (p)->p3 = 0
	
/*
 * RECT16angle:
 */
#pragma pack(push,1)

typedef struct _RECT16 {
	short x, y;		/* offset point on VRAM */
	short w, h;		/* width and height */
} RECT16;

// Psy-X custom struct to handle polygons

#if USE_EXTENDED_PRIM_POINTERS

#if defined(_M_X64) || defined(__amd64__)

#define DECLARE_P_ADDR \
		uintptr_t addr; \
		uint len : 16; \
		uint pgxp_index : 16;

#define P_LEN		3		// 3 longs

#else

#define DECLARE_P_ADDR \
		uintptr_t addr; \
		uint len : 16; \
		uint pgxp_index : 16;

#define P_LEN		2		// 2 longs

#endif // _M_X64 || __amd64__

#define DECLARE_P_ADDR_PTAG DECLARE_P_ADDR

#else // just don't use that, okay... it's just for reference

#define DECLARE_P_ADDR_PTAG \
	unsigned addr : 24; \
	unsigned len : 8;

#define DECLARE_P_ADDR \
	u_int tag;

#define P_LEN		1		// 1 long

#endif

/*
 * Polygon Primitive Definitions
 */

typedef struct {
	DECLARE_P_ADDR_PTAG
} OT_TAG;

typedef struct {
	DECLARE_P_ADDR_PTAG
	u_char	pad0, pad1, pad2, code;
} P_TAG;
		
typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, y0;
	VERTTYPE	x1,	y1;
	VERTTYPE	x2,	y2;
} POLY_F3;				/* Flat Triangle */

static_assert(sizeof(POLY_F3) / 4 - P_LEN == 4, "POLY_F3 size must be 4 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, y0;
	VERTTYPE	x1,	y1;
	VERTTYPE	x2,	y2;
	VERTTYPE	x3,	y3;
} POLY_F4;				/* Flat Quadrangle */

static_assert(sizeof(POLY_F4) / 4 - P_LEN == 5, "POLY_F4 size must be 5 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
	VERTTYPE	x1,	y1;
	u_char	u1, v1;	u_short	tpage;
	VERTTYPE	x2,	y2;
	u_char	u2, v2;	u_short	pad1;
} POLY_FT3;				/* Flat Textured Triangle */

static_assert(sizeof(POLY_FT3) / 4 - P_LEN == 7, "POLY_FT3 size must be 7 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
	VERTTYPE	x1,	y1;
	u_char	u1, v1;	u_short	tpage;
	VERTTYPE	x2,	y2;
	u_char	u2, v2;	u_short	pad1;
	VERTTYPE	x3,	y3;
	u_char	u3, v3;	u_short	pad2;
} POLY_FT4;				/* Flat Textured Quadrangle */

static_assert(sizeof(POLY_FT4) / 4 - P_LEN == 9, "POLY_FT4 size must be 9 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	r1, g1, b1, pad1;
	VERTTYPE	x1,	y1;
	u_char	r2, g2, b2, pad2;
	VERTTYPE	x2,	y2;
} POLY_G3;				/* Gouraud Triangle */

static_assert(sizeof(POLY_G3) / 4 - P_LEN == 6, "POLY_G3 size must be 6 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	r1, g1, b1, pad1;
	VERTTYPE	x1,	y1;
	u_char	r2, g2, b2, pad2;
	VERTTYPE	x2,	y2;
	u_char	r3, g3, b3, pad3;
	VERTTYPE	x3,	y3;
} POLY_G4;				/* Gouraud Quadrangle */

static_assert(sizeof(POLY_G4) / 4 - P_LEN == 8, "POLY_G4 size must be 8 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
	u_char	r1, g1, b1, p1;
	VERTTYPE	x1,	y1;
	u_char	u1, v1;	u_short	tpage;
	u_char	r2, g2, b2, p2;
	VERTTYPE	x2,	y2;
	u_char	u2, v2;	u_short	pad2;
} POLY_GT3;				/* Gouraud Textured Triangle */

static_assert(sizeof(POLY_GT3) / 4 - P_LEN == 9, "POLY_GT3 size must be 9 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
	u_char	r1, g1, b1, p1;
	VERTTYPE	x1,	y1;
	u_char	u1, v1;	u_short	tpage;
	u_char	r2, g2, b2, p2;
	VERTTYPE	x2,	y2;
	u_char	u2, v2;	u_short	pad2;
	u_char	r3, g3, b3, p3;//10
	VERTTYPE	x3,	y3;//11
	u_char	u3, v3;	u_short	pad3;
} POLY_GT4;				/* Gouraud Textured Quadrangle */

static_assert(sizeof(POLY_GT4) / 4 - P_LEN == 12, "POLY_GT4 size must be 12 longs");

/*
 * Line Primitive Definitions
 */
typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	VERTTYPE	x1,	y1;
} LINE_F2;				/* Unconnected Flat Line */

static_assert(sizeof(LINE_F2) / 4 - P_LEN == 3, "LINE_F2 size must be 3 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	r1, g1, b1, p1;
	VERTTYPE	x1,	y1;
} LINE_G2;				/* Unconnected Gouraud Line */

static_assert(sizeof(LINE_G2) / 4 - P_LEN == 4, "LINE_G2 size must be 4 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	VERTTYPE	x1,	y1;
	VERTTYPE	x2,	y2;
	u_int	pad;
} LINE_F3;				/* 2 connected Flat Line */

static_assert(sizeof(LINE_F3) / 4 - P_LEN == 5, "LINE_F3 size must be 5 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	r1, g1, b1, p1;
	VERTTYPE	x1,	y1;
	u_char	r2, g2, b2, p2;
	VERTTYPE	x2,	y2;
	u_int	pad;
} LINE_G3;				/* 2 connected Gouraud Line */

static_assert(sizeof(LINE_G3) / 4 - P_LEN == 7, "LINE_G3 size must be 7 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	VERTTYPE	x1,	y1;
	VERTTYPE	x2,	y2;
	VERTTYPE	x3,	y3;
	u_int	pad;
} LINE_F4;				/* 3 connected Flat Line Quadrangle */

static_assert(sizeof(LINE_F4) / 4 - P_LEN == 6, "LINE_F4 size must be 6 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	r1, g1, b1, p1;
	VERTTYPE	x1,	y1;
	u_char	r2, g2, b2, p2;
	VERTTYPE	x2,	y2;
	u_char	r3, g3, b3, p3;
	VERTTYPE	x3,	y3;
	u_int	pad;
} LINE_G4;				/* 3 connected Gouraud Line */

static_assert(sizeof(LINE_G4) / 4 - P_LEN == 9, "LINE_G4 size must be 9 longs");

/*
 * Sprite Primitive Definitions
 */
typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
	VERTTYPE	w,	h;
} SPRT;					/* free size Sprite */

static_assert(sizeof(SPRT) / 4 - P_LEN == 4, "SPRT size must be 4 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
} SPRT_16;				/* 16x16 Sprite */

static_assert(sizeof(SPRT_16) / 4 - P_LEN == 3, "SPRT_16 size must be 3 longs");
		   
typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
	u_char	u0, v0;	u_short	clut;
} SPRT_8;				/* 8x8 Sprite */

static_assert(sizeof(SPRT_8) / 4 - P_LEN == 3, "SPRT_8 size must be 3 longs");
		   
/*
 * Tile Primitive Definitions
 */
typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, y0;
	VERTTYPE	w,	h;
} TILE;					/* free size Tile */

static_assert(sizeof(TILE) / 4 - P_LEN == 3, "TILE size must be 3 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
} TILE_16;				/* 16x16 Tile */

static_assert(sizeof(TILE_16) / 4 - P_LEN == 2, "TILE_16 size must be 2 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
} TILE_8;				/* 8x8 Tile */

static_assert(sizeof(TILE_8) / 4 - P_LEN == 2, "TILE_8 size must be 2 longs");

typedef struct {
	DECLARE_P_ADDR
	u_char	r0, g0, b0, code;
	VERTTYPE	x0, 	y0;
} TILE_1;				/* 1x1 Tile */

static_assert(sizeof(TILE_1) / 4 - P_LEN == 2, "TILE_1 size must be 2 longs");

/*
 *  Special Primitive Definitions
 */
typedef struct {
	DECLARE_P_ADDR
	u_int	code[2];
} DR_MODE;				/* Drawing Mode */

static_assert(sizeof(DR_MODE) / 4 - P_LEN == 2, "DR_MODE size must be 2 longs");

typedef struct {
	DECLARE_P_ADDR
	u_int	code[2];
} DR_TWIN;				/* Texture Window */

static_assert(sizeof(DR_TWIN) / 4 - P_LEN == 2, "DR_TWIN size must be 2 longs");
		   
typedef struct {
	DECLARE_P_ADDR
	u_int	code[2];
} DR_AREA;				/* Drawing Area */

static_assert(sizeof(DR_AREA) / 4 - P_LEN == 2, "DR_AREA size must be 2 longs");
		   
typedef struct {
	DECLARE_P_ADDR
	u_int	code[2];
} DR_OFFSET;				/* Drawing Offset */

static_assert(sizeof(DR_OFFSET) / 4 - P_LEN == 2, "DR_OFFSET size must be 2 longs");
		   
typedef struct {			/* MoveImage */
	DECLARE_P_ADDR
	u_int	code[5];
} DR_MOVE;

static_assert(sizeof(DR_MOVE) / 4 - P_LEN == 5, "DR_MOVE size must be 5 longs");

typedef struct {			/* LoadImage */
	DECLARE_P_ADDR
	u_int	code[3];
	u_int	p[13];
} DR_LOAD;

static_assert(sizeof(DR_LOAD) / 4 - P_LEN == 16, "DR_LOAD size must be 16 longs");

typedef	struct {
	DECLARE_P_ADDR
	u_int	code[1];
} DR_TPAGE;				/* Drawing TPage */

static_assert(sizeof(DR_TPAGE) / 4 - P_LEN == 1, "DR_TPAGE size must be 1 long");

typedef struct {
	DECLARE_P_ADDR
	u_int  code[2];
} DR_STP;                               /* Drawing STP */

static_assert(sizeof(DR_STP) / 4 - P_LEN == 2, "DR_STP size must be 2 longs");

/* 
* PSY-X commands
*/

typedef struct {
	DECLARE_P_ADDR
	u_int  code[2];
} DR_PSYX_TEX;

static_assert(sizeof(DR_PSYX_TEX) / 4 - P_LEN == 2, "DR_PSYX_TEX size must be 2 longs");

typedef struct {
	DECLARE_P_ADDR
	u_int  code;
	const char* text;
} DR_PSYX_DBGMARKER;

/*
 * Environment
 */
typedef struct {
	DECLARE_P_ADDR
	u_int	code[15];
} DR_ENV;				/* Packed Drawing Environment */

static_assert(sizeof(DR_ENV) / 4 - P_LEN == 15, "DR_ENV size must be 15 longs");

typedef struct {
	RECT16	clip;		/* clip area */
	short	ofs[2];		/* drawing offset */
	RECT16	tw;		/* texture window */
	u_short tpage;		/* texture page */
	u_char	dtd;		/* dither flag (0:off, 1:on) */
	u_char	dfe;		/* flag to draw on display area (0:off 1:on) */
	u_char	drt;		
	u_char	isbg;		/* enable to auto-clear */
	u_char	r0, g0, b0;	/* initital background color */
	DR_ENV	dr_env;		/* reserved */
} DRAWENV;

typedef struct {
	RECT16	disp;		/* display area */
	RECT16	screen;		/* display start point */
	u_char	isinter;	/* interlace 0: off 1: on */
	u_char	isrgb24;	/* RGB24 bit mode */
	u_char	pad0, pad1;	/* reserved */
} DISPENV;

/*
 *	Font Stream Parameters
 */
#define FNT_MAX_ID	8	/* max number of stream ID */
#define FNT_MAX_SPRT	1024	/* max number of sprites in all streams */

/*
 *	Multi-purpose Sony-TMD primitive
 */
typedef struct {
	u_int	id;
	u_char	r0, g0, b0, p0;		/* Color of vertex 0 */
	u_char	r1, g1, b1, p1;		/* Color of vertex 1 */
	u_char	r2, g2, b2, p2;		/* Color of vertex 2 */
	u_char	r3, g3, b3, p3;		/* Color of vertex 3 */
	u_short	tpage, clut;		/* texture page ID, clut ID */
	u_char	u0, v0, u1, v1;		/* texture corner point */
	u_char	u2, v2, u3, v3;
	
	/* independent vertex model */
	SVECTOR	x0, x1, x2, x3;		/* 3D corner point */
	SVECTOR	n0, n1, n2, n3;		/* 3D corner normal vector */
	
	/* Common vertex model */
	SVECTOR	*v_ofs;			/* offset to vertex database */
	SVECTOR	*n_ofs;			/* offset to normal database */
	
	u_short	vert0, vert1; 		/* index of vertex */
	u_short	vert2, vert3;		
	u_short	norm0, norm1; 		/* index of normal */
	u_short	norm2, norm3;

	
} TMD_PRIM;

/*
 *	Multi-purpose TIM image
 */
typedef struct {
	u_int	mode;		/* pixel mode */
	RECT16	*cRECT16;		/* CLUT RECT16angle on frame buffer */
	u_int*	caddr;		/* CLUT address on main memory */
	RECT16	*pRECT16;		/* texture image RECT16angle on frame buffer */
	u_int*	paddr;		/* texture image address on main memory */
} TIM_IMAGE;

#pragma pack(pop)

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

#if USE_EXTENDED_PRIM_POINTERS
extern OT_TAG prim_terminator;
#endif

#ifdef LoadImage
#undef LoadImage
#endif
	
/*
 * Prototypes
 */
#ifndef _FNTPRINT_
#define _FNTPRINT_
extern int FntPrint(char* fmt, ...);
#endif /* _FNTPRINT_ */
#ifndef _KANJIFNTPRINT_
#define _KANJIFNTPRINT_
extern int KanjiFntPrint(char* text, ...);
#endif /* _KANJIFNTPRINT_ */
	
extern DISPENV *GetDispEnv(DISPENV *env);
extern DISPENV *PutDispEnv(DISPENV *env);
extern DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h);
extern DRAWENV *GetDrawEnv(DRAWENV *env);
extern DRAWENV *PutDrawEnv(DRAWENV *env);
extern DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h);
extern TIM_IMAGE *ReadTIM(TIM_IMAGE *timimg);
extern TMD_PRIM *ReadTMD(TMD_PRIM *tmdprim);
extern int CheckPrim(char *s, u_long *p);
extern int ClearImage(RECT16 *RECT16, u_char r, u_char g, u_char b);
extern int ClearImage2(RECT16 *RECT16, u_char r, u_char g, u_char b);
extern int DrawSync(int mode);
extern int FntOpen(int x, int y, int w, int h, int isbg, int n);
extern int GetGraphDebug(void);
extern int GetTimSize(u_char *sjis);
extern int IsEndPrim(void *p);
extern int KanjiFntOpen(int x, int y, int w, int h, int dx, int dy, int cx, int cy, int isbg, int n);
extern void KanjiFntClose(void);
extern int Krom2Tim(u_char *sjis, u_long* taddr, int dx, int dy, int cdx, int cdy, u_int fg, u_int bg);
extern int LoadImage(RECT16* rect, u_long* p);
extern int MargePrim(void *p0, void *p1);
extern int MoveImage(RECT16* rect, int x, int y);
extern int OpenTIM(u_long *addr);
extern int OpenTMD(u_long *tmd, int obj_no);
extern int ResetGraph(int mode);
extern int SetGraphDebug(int level);
extern int StoreImage(RECT16 *rect, u_long *p);
extern u_long* ClearOTag(u_long*ot, int n);
extern u_long* ClearOTagR(u_long*ot, int n);
extern u_long* FntFlush();
extern u_long* KanjiFntFlush(int id);
extern u_int DrawSyncCallback(void (*func)(void));
extern u_short GetClut(int x, int y);
extern u_short GetTPage(int tp, int abr, int x, int y);
extern u_short LoadClut(u_long*clut, int x, int y);
extern u_short LoadClut2(u_long*clut, int x, int y);
extern u_short LoadTPage(u_long*pix, int tp, int abr, int x, int y, int w, int h);
extern void *NextPrim(void *p);
extern void AddPrim(void *ot, void *p);
extern void AddPrims(void *ot, void *p0, void *p1);
extern void CatPrim(void *p0, void *p1);
extern void DrawOTag(u_long *p);
extern void DrawOTagIO(u_long *p);
extern void DrawOTagEnv(u_long *p, DRAWENV *env);
extern void DrawPrim(void *p);
extern void DumpClut(u_short clut);
extern void DumpDispEnv(DISPENV *env);
extern void DumpDrawEnv(DRAWENV *env);
extern void DumpOTag(u_long *p);
extern void DumpTPage(u_short tpage);
extern void FntLoad(int x, int y);
extern void SetDispMask(int mask);
extern void SetDrawArea(DR_AREA *p, RECT16 *r);
extern void SetDrawEnv(DR_ENV *dr_env, DRAWENV *env);
extern void SetDrawLoad(DR_LOAD *p, RECT16 *RECT16);
extern void SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT16 *tw);
extern void SetDrawTPage(DR_TPAGE *p, int dfe, int dtd, int tpage);
extern void SetDrawMove(DR_MOVE *p, RECT16 *RECT16, int x, int y);
extern void SetDrawOffset(DR_OFFSET *p, u_short *ofs);
extern void SetDrawStp(DR_STP *p, int pbw);
extern void SetDumpFnt(int id);
extern void SetLineF2(LINE_F2 *p);
extern void SetLineF3(LINE_F3 *p);
extern void SetLineF4(LINE_F4 *p);
extern void SetLineG2(LINE_G2 *p);
extern void SetLineG3(LINE_G3 *p);
extern void SetLineG4(LINE_G4 *p);
extern void SetPolyF3(POLY_F3 *p);
extern void SetPolyF4(POLY_F4 *p);
extern void SetPolyFT3(POLY_FT3 *p);
extern void SetPolyFT4(POLY_FT4 *p);
extern void SetPolyG3(POLY_G3 *p);
extern void SetPolyG4(POLY_G4 *p);
extern void SetPolyGT3(POLY_GT3 *p);
extern void SetPolyGT4(POLY_GT4 *p);
extern void SetSemiTrans(void *p, int abe);
extern void SetShadeTex(void *p, int tge);
extern void SetSprt(SPRT *p);
extern void SetSprt16(SPRT_16 *p);
extern void SetSprt8(SPRT_8 *p);
extern void SetTexWindow(DR_TWIN *p, RECT16 *tw);
extern void SetTile(TILE *p);
extern void SetTile1(TILE_1 *p);
extern void SetTile16(TILE_16 *p);
extern void SetTile8(TILE_8 *p);
extern void TermPrim(void *p);
extern u_long* BreakDraw(void);
extern void ContinueDraw(u_int*insaddr, u_int*contaddr);
extern int IsIdleGPU(int max_count);
extern int GetODE(void);
extern int LoadImage2(RECT16 *RECT16, u_long *p);
extern int StoreImage2(RECT16 *RECT16, u_long *p);
extern int MoveImage2(RECT16 *RECT16, int x, int y);
extern int DrawOTag2(u_int*p);
extern void GetDrawMode(DR_MODE *p);
extern void GetTexWindow(DR_TWIN *p);
extern void GetDrawArea(DR_AREA *p);
extern void GetDrawOffset(DR_OFFSET *p);
extern void GetDrawEnv2(DR_ENV *p);

/*
* PSY-X commands
*/

extern void SetPsyXTexture(DR_PSYX_TEX *p, uint grTextureId, int width, int height);
extern void SetPsyXDebugMarker(DR_PSYX_DBGMARKER* p, const char* str);

#ifdef _DEBUG
#define PSYX_DBG_MARKER_TEXT(primptr, ot, text) \
	{ \
		DR_PSYX_DBGMARKER* marker; \
		marker = (DR_PSYX_DBGMARKER*)(primptr); \
		SetPsyXDebugMarker(marker, text); \
		(primptr) += sizeof(DR_PSYX_DBGMARKER); \
		addPrim((ot), marker); \
	}
#else
#define PSYX_DBG_MARKER_TEXT(primptr, ot, text)
#endif

#define PSYX_DBG_MARKER_RESET(primptr, ot) PSYX_DBG_MARKER_TEXT(primptr, ot, nullptr)

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif
