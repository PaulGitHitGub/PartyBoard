#ifndef PARTYBOARD_SHARED_MATH_H
#define PARTYBOARD_SHARED_MATH_H
/* Force-included only in game C and Aurora matrix C. The port UI and
 * dependencies keep their system maths. sqrtf retains msl_sqrtf.h semantics;
 * positive square roots use the hardware/system IEEE operation. */
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
double pbd_sin(double x);
float pbd_sinf(float x);
double pbd_cos(double x);
float pbd_cosf(float x);
double pbd_tan(double x);
float pbd_tanf(float x);
double pbd_atan(double x);
float pbd_atanf(float x);
double pbd_asin(double x);
float pbd_asinf(float x);
double pbd_acos(double x);
float pbd_acosf(float x);
double pbd_pow(double x, double y);
float pbd_powf(float x, float y);
double pbd_atan2(double x, double y);
float pbd_atan2f(float x, float y);
double pbd_fmod(double x, double y);
float pbd_fmodf(float x, float y);
#ifdef __cplusplus
}
#endif
#define sin pbd_sin
#define sinf pbd_sinf
#define cos pbd_cos
#define cosf pbd_cosf
#define tan pbd_tan
#define tanf pbd_tanf
#define atan pbd_atan
#define atanf pbd_atanf
#define asin pbd_asin
#define asinf pbd_asinf
#define acos pbd_acos
#define acosf pbd_acosf
#define pow pbd_pow
#define powf pbd_powf
#define atan2 pbd_atan2
#define atan2f pbd_atan2f
#define fmod pbd_fmod
#define fmodf pbd_fmodf
#endif
