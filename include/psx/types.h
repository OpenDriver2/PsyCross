#ifndef	TYPES_H
#define	TYPES_H

#include <stdint.h>
#include <stddef.h>

/*
 * Pre-empt macOS/BSD <sys/types.h> from declaring u_long as `unsigned long`
 * (which is 64-bit on LP64). PSX code expects u_long to be 32-bit, so we
 * mark the system's guard and define u_long ourselves below.
 */
#if defined(__APPLE__) && (defined(__LP64__) || defined(_LP64))
#  ifndef _U_LONG
#    define _U_LONG
#  endif
#  ifndef _U_INT
#    define _U_INT
#  endif
#  ifndef _U_CHAR
#    define _U_CHAR
#  endif
#  ifndef _U_SHORT
#    define _U_SHORT
#  endif
#endif

#if !defined(__APPLE__)
/* major part of a device */
#define	major(x)	((int)(((unsigned)(x)>>8)&0377))

/* minor part of a device */
#define	minor(x)	((int)((x)&0377))

/* make a device number */
#define	makedev(x,y)	((dev_t)(((x)<<8) | (y)))

#endif

#ifndef _UCHAR_T
#define _UCHAR_T
typedef	unsigned char	u_char;
#endif
#ifndef _USHORT_T
#define _USHORT_T
typedef	unsigned short	u_short;
#endif
#ifndef _UINT_T
#define _UINT_T
typedef	unsigned int	u_int;
#endif
#ifndef _ULONG_T
#define _ULONG_T
/*
 * The original PSX SDK treats `u_long` as a 32-bit type. On LP64 platforms
 * (macOS arm64/x64, Linux x64) `unsigned long` is 64-bit, which breaks the
 * PSX-style packing/casting throughout the codebase. Map it to uint32_t on
 * those targets to preserve binary layout and call-site compatibility.
 */
#if defined(__LP64__) || defined(_LP64)
typedef	uint32_t	u_long;
#else
typedef	unsigned long	u_long;
#endif
#endif
#ifndef _SYSIII_USHORT
#define _SYSIII_USHORT
typedef	unsigned short	ushort;		/* sys III compat */
#endif
#ifndef __psx__
#ifndef _SYSV_UINT
#define _SYSV_UINT
typedef	unsigned int	uint;		/* sys V compat */
#endif
#ifndef _SYSV_ULONG
#define _SYSV_ULONG
#if defined(__LP64__) || defined(_LP64)
typedef	uint32_t	ulong;		/* sys V compat */
#else
typedef	unsigned long	ulong;		/* sys V compat */
#endif
#endif
#endif /* ! __psx__ */

#ifndef NBBY
#define	NBBY	8
#endif

#endif
