#ifndef PARTYBOARD_DETERMINISM_H
#define PARTYBOARD_DETERMINISM_H

/*
 * Cross-play determinism: explicit versions of the few operations whose result
 * the C language leaves undefined and that an Apple chip (arm64) and an x86-64 PC
 * really do compute differently. Online play runs the whole game on every
 * computer and only sends controller presses, so both must get the same answer.
 *
 *   - Converting a float to a 32/64-bit integer when the value is NaN or out of
 *     range: arm64 saturates (NaN gives 0, a negative value to an unsigned type
 *     gives 0, a too-large value gives the maximum); x86 gives 0x80000000 or
 *     wraps around. Conversions to 8- and 16-bit types go through a signed 32-bit
 *     conversion on both and already agree (except for infinities).
 *   - Integer division or remainder by zero: arm64 gives 0 (remainder: the
 *     dividend); x86 stops the program. INT_MIN / -1 likewise.
 *
 * Every helper returns exactly what the arm64 build has always produced, so the
 * Mac game -- and its lockstep with released Mac copies -- is unchanged, and the
 * Windows build now produces the same. When a helper meets one of those values it
 * writes a line to game.log (a few times per place in the code, see
 * src/port/determinism.c), so a desync investigation can see that it happened.
 *
 * Use through the macros: PB_F2S32(x), PB_F2U32(x), PB_F2U64(x), PB_DIV(a, b),
 * PB_MOD(a, b), PB_UDIV(a, b), PB_UMOD(a, b); PBDetQuietNaNf() for uninitialised floats.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void PartyBoard_DeterminismNote(const char *file, int line, const char *what, double value);
#ifdef __cplusplus
}
#endif

static inline int32_t PBDetF2S32(double x, const char *file, int line)
{
    if (x != x) {
        PartyBoard_DeterminismNote(file, line, "NaN converted to int, using 0", x);
        return 0;
    }
    if (x >= 2147483648.0) {
        PartyBoard_DeterminismNote(file, line, "too large for int, using INT_MAX", x);
        return INT32_MAX;
    }
    if (x <= -2147483649.0) {
        PartyBoard_DeterminismNote(file, line, "too small for int, using INT_MIN", x);
        return INT32_MIN;
    }
    return (int32_t)x;
}

static inline uint32_t PBDetF2U32(double x, const char *file, int line)
{
    if (x != x) {
        PartyBoard_DeterminismNote(file, line, "NaN converted to unsigned, using 0", x);
        return 0;
    }
    if (x <= -1.0) {
        PartyBoard_DeterminismNote(file, line, "negative converted to unsigned, using 0", x);
        return 0;
    }
    if (x >= 4294967296.0) {
        PartyBoard_DeterminismNote(file, line, "too large for unsigned, using UINT_MAX", x);
        return UINT32_MAX;
    }
    return (uint32_t)x;
}

/* 64-bit unsigned: the game's pointer-sized work[] slots (u32 on the console). */
static inline uint64_t PBDetF2U64(double x, const char *file, int line)
{
    if (x != x) {
        PartyBoard_DeterminismNote(file, line, "NaN converted to unsigned, using 0", x);
        return 0;
    }
    if (x <= -1.0) {
        PartyBoard_DeterminismNote(file, line, "negative converted to unsigned, using 0", x);
        return 0;
    }
    if (x >= 18446744073709551616.0) {
        PartyBoard_DeterminismNote(file, line, "too large for unsigned, using the maximum", x);
        return UINT64_MAX;
    }
    return (uint64_t)x;
}

static inline int32_t PBDetDivS32(int32_t a, int32_t b, const char *file, int line)
{
    if (b == 0) {
        PartyBoard_DeterminismNote(file, line, "integer division by zero, using 0", a);
        return 0;
    }
    if (b == -1 && a == INT32_MIN) {
        PartyBoard_DeterminismNote(file, line, "INT_MIN / -1, using INT_MIN", a);
        return INT32_MIN;
    }
    return a / b;
}

static inline int32_t PBDetModS32(int32_t a, int32_t b, const char *file, int line)
{
    if (b == 0) {
        PartyBoard_DeterminismNote(file, line, "integer remainder by zero, using the dividend", a);
        return a;
    }
    if (b == -1) {
        return 0; /* also INT_MIN % -1, which stops an x86 program */
    }
    return a % b;
}

static inline uint32_t PBDetDivU32(uint32_t a, uint32_t b, const char *file, int line)
{
    if (b == 0) {
        PartyBoard_DeterminismNote(file, line, "integer division by zero, using 0", a);
        return 0;
    }
    return a / b;
}

static inline uint32_t PBDetModU32(uint32_t a, uint32_t b, const char *file, int line)
{
    if (b == 0) {
        PartyBoard_DeterminismNote(file, line, "integer remainder by zero, using the dividend", a);
        return a;
    }
    return a % b;
}

/*
 * The float NaN with bit pattern 0x7fc00000. Where the decompiled code reads an
 * uninitialised float, the Mac compiler (clang -O2) substitutes exactly this
 * value; initialising with it keeps the Mac behaviour and gives the PC the same.
 * Built from bits because MSVC's NAN macro has the sign bit set.
 */
static inline float PBDetQuietNaNf(void)
{
    union {
        uint32_t u;
        float f;
    } v;
    v.u = 0x7fc00000u;
    return v.f;
}

/*
 * (rand8() << 8) | rand8(): two draws from the game's random generator in one
 * expression. C does not say which call runs first, so another compiler may put
 * the draws the other way round. The Mac build (clang) and the console make the
 * FIRST draw the high byte; this does exactly that, in a fixed order.
 */
int32_t rand8(void);
static inline int32_t PBDetRand8Pair(void)
{
    int32_t high = rand8();
    int32_t low = rand8();
    return (high << 8) | low;
}

/*
 * Float to an 8- or 16-bit integer. The GameCube converted through a 32-bit integer and kept its low
 * bits (so -2.5 to u8 is 254), and so does an x86 PC. When the value does not fit the small type the
 * conversion is undefined in C, and clang on the Mac may skip the narrowing and keep the 32-bit value
 * in a register (m415: a grid index of -2 instead of 254). These do the GameCube's conversion
 * explicitly, so all three agree, and log when a value did not fit.
 */
static inline int32_t PBDetF2Small(double x, double lo, double hi, const char *file, int line)
{
    int32_t v = PBDetF2S32(x, file, line);
    if (!(x > lo - 1.0 && x < hi + 1.0)) {
        PartyBoard_DeterminismNote(file, line, "value does not fit the small integer type, kept its low bits like the GameCube", x);
    }
    return v;
}
#define PB_F2S8(x) ((int8_t)PBDetF2Small((x), -128.0, 127.0, __FILE__, __LINE__))
#define PB_F2U8(x) ((uint8_t)PBDetF2Small((x), 0.0, 255.0, __FILE__, __LINE__))
#define PB_F2S16(x) ((int16_t)PBDetF2Small((x), -32768.0, 32767.0, __FILE__, __LINE__))
#define PB_F2U16(x) ((uint16_t)PBDetF2Small((x), 0.0, 65535.0, __FILE__, __LINE__))

/* A once-per-run game.log line for a fixed 64-bit layout problem (see docs/DETERMINISM.md). */
#define PB_NOTE_ONCE(what) \
    do { \
        static int pbNotedOnce_; \
        if (!pbNotedOnce_) { \
            pbNotedOnce_ = 1; \
            PartyBoard_DeterminismNote(__FILE__, __LINE__, (what), 0); \
        } \
    } while (0)

#define PB_F2S32(x) PBDetF2S32((x), __FILE__, __LINE__)
#define PB_F2U32(x) PBDetF2U32((x), __FILE__, __LINE__)
#define PB_F2U64(x) PBDetF2U64((x), __FILE__, __LINE__)
#define PB_DIV(a, b) PBDetDivS32((a), (b), __FILE__, __LINE__)
#define PB_MOD(a, b) PBDetModS32((a), (b), __FILE__, __LINE__)
#define PB_UDIV(a, b) PBDetDivU32((a), (b), __FILE__, __LINE__)
#define PB_UMOD(a, b) PBDetModU32((a), (b), __FILE__, __LINE__)

#endif
