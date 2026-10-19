#include "pb_fdlibm.h"
float pbd_sinf(float x) { return (float)pbd_sin(x); }
float pbd_cosf(float x) { return (float)pbd_cos(x); }
float pbd_tanf(float x) { return (float)pbd_tan(x); }
float pbd_atanf(float x) { return (float)pbd_atan(x); }
double pbd_asin(double x) { return pbd_ieee754_asin(x); }
float pbd_asinf(float x) { return (float)pbd_asin(x); }
double pbd_acos(double x) { return pbd_ieee754_acos(x); }
float pbd_acosf(float x) { return (float)pbd_acos(x); }
double pbd_pow(double x, double y) { return pbd_ieee754_pow(x,y); }
float pbd_powf(float x, float y) { return (float)pbd_pow(x,y); }
double pbd_atan2(double x, double y) { return pbd_ieee754_atan2(x,y); }
float pbd_atan2f(float x, float y) { return (float)pbd_atan2(x,y); }
double pbd_fmod(double x, double y) { return pbd_ieee754_fmod(x,y); }
float pbd_fmodf(float x, float y) { return (float)pbd_fmod(x,y); }
