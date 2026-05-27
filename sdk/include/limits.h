#ifndef _LIMITS_H
#define _LIMITS_H
/* Minimal limits.h for the EquinoxOS userspace SDK.
 *
 * We only define what vendored libraries actually use. So far the only
 * consumer is BearSSL's inner.h which probes ULONG_MAX to pick the
 * 32/64-bit code path. Everything below targets x86_64 (LP64 on Linux,
 * LLP64 on Windows — we're LP64-like since our SDK uses ELF and `long`
 * is 64-bit under x86_64-elf-gcc).
 */

#define CHAR_BIT      8
#define SCHAR_MIN     (-128)
#define SCHAR_MAX     127
#define UCHAR_MAX     255
#define CHAR_MIN      SCHAR_MIN
#define CHAR_MAX      SCHAR_MAX

#define SHRT_MIN      (-32768)
#define SHRT_MAX      32767
#define USHRT_MAX     65535

#define INT_MIN       (-INT_MAX - 1)
#define INT_MAX       2147483647
#define UINT_MAX      4294967295U

#define LONG_MIN      (-LONG_MAX - 1L)
#define LONG_MAX      9223372036854775807L
#define ULONG_MAX     18446744073709551615UL

#define LLONG_MIN     (-LLONG_MAX - 1LL)
#define LLONG_MAX     9223372036854775807LL
#define ULLONG_MAX    18446744073709551615ULL

#endif /* _LIMITS_H */
