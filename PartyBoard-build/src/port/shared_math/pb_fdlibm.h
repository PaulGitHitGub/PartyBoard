#ifndef PARTYBOARD_PRIVATE_FDLIBM_H
#define PARTYBOARD_PRIVATE_FDLIBM_H
#include <math.h>
#include <stdint.h>
#include <errno.h>
#define __HI(x) (*((int32_t *)&(x)+1))
#define __LO(x) (*((int32_t *)&(x)))
#define __HIp(x) (*((int32_t *)(x)+1))
#define __LOp(x) (*((int32_t *)(x)))
static inline double fabs__Fd(double x) { return fabs(x); }
#define sin pbd_sin
#define cos pbd_cos
#define tan pbd_tan
#define atan pbd_atan
#define floor pbd_floor
#define copysign pbd_copysign
#define ldexp pbd_ldexp
#define frexp pbd_frexp
#define modf pbd_modf
#define __ieee754_asin pbd_ieee754_asin
#define __ieee754_acos pbd_ieee754_acos
#define __ieee754_atan2 pbd_ieee754_atan2
#define __ieee754_pow pbd_ieee754_pow
#define __ieee754_fmod pbd_ieee754_fmod
#define __ieee754_rem_pio2 pbd_ieee754_rem_pio2
#define __kernel_sin pbd_kernel_sin
#define __kernel_cos pbd_kernel_cos
#define __kernel_tan pbd_kernel_tan
#define __kernel_rem_pio2 pbd_kernel_rem_pio2
double sin(double); double cos(double); double tan(double); double atan(double);
double floor(double); double copysign(double,double); double ldexp(double,int);
double frexp(double,int*); double modf(double,double*);
double __ieee754_asin(double); double __ieee754_acos(double); double __ieee754_atan2(double,double);
double __ieee754_pow(double,double); double __ieee754_fmod(double,double);
int __ieee754_rem_pio2(double,double*);
double __kernel_sin(double,double,int); double __kernel_cos(double,double); double __kernel_tan(double,double,int);
int __kernel_rem_pio2(double*,double*,int,int,int,const int*);

#endif
