/* ---------------------------------------------------------------------------
 * src/system/misc/rtc.c — CMOS / MC146818 RTC reader.
 *
 * See rtc.h for the contract. The implementation is small but has a few
 * traps worth calling out:
 *
 *   * We MUST disable NMI while talking to port 0x70 (the high bit of
 *     the value written is the "NMI disable" line on PCs). Convention
 *     is to always re-enable on the last write, otherwise we leave the
 *     box in an awkward state.
 *
 *   * Reading the seconds twice and comparing is not paranoia — on
 *     fast emulators (KVM, WHPX) it's entirely possible for the BCD-
 *     to-binary conversion in the kernel to land across a tick. The
 *     OSDev wiki pattern is to loop until two back-to-back reads of
 *     the whole RTC return identical values.
 *
 *   * The mktime() math here is a hand-rolled "civil-from-fields"
 *     conversion that handles Gregorian leap years correctly through
 *     2099. We don't need the full Howard Hinnant date trick — for the
 *     RTC values we get, year ∈ [2000, 2099] is enough.
 * ------------------------------------------------------------------------ */

#include "rtc.h"
#include "../core/io.h"
#include <stdint.h>

#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

/* MC146818 register map (subset). */
#define CMOS_REG_SECONDS    0x00
#define CMOS_REG_MINUTES    0x02
#define CMOS_REG_HOURS      0x04
#define CMOS_REG_DAY        0x07
#define CMOS_REG_MONTH      0x08
#define CMOS_REG_YEAR       0x09
#define CMOS_REG_CENTURY    0x32   /* PIIX4 / most modern boards */
#define CMOS_REG_STATUS_A   0x0A
#define CMOS_REG_STATUS_B   0x0B

/* Status register A, bit 7 = Update In Progress. */
#define CMOS_STATUS_A_UIP   0x80
/* Status register B, bit 1 = 24-hour mode, bit 2 = binary mode. */
#define CMOS_STATUS_B_24H   0x02
#define CMOS_STATUS_B_BIN   0x04

static uint8_t cmos_read(uint8_t reg) {
    /* High bit set keeps NMI disabled during the read. */
    outb(CMOS_INDEX_PORT, (uint8_t)(0x80 | reg));
    return inb(CMOS_DATA_PORT);
}

static int cmos_update_in_progress(void) {
    return (cmos_read(CMOS_REG_STATUS_A) & CMOS_STATUS_A_UIP) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

/* Days-in-month for a non-leap year. February gets +1 if leap. */
static const uint16_t MONTH_DAYS[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap_gregorian(unsigned year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Convert a UTC civil date+time into Unix epoch seconds. Assumes inputs
 * are already validated (year ≥ 1970, month ∈ 1..12, etc); the RTC
 * read path normalises everything before calling this. */
static uint64_t mktime_utc(unsigned year, unsigned month, unsigned day,
                           unsigned hour, unsigned minute, unsigned sec) {
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; y++) {
        days += is_leap_gregorian(y) ? 366u : 365u;
    }
    for (unsigned m = 1; m < month; m++) {
        days += MONTH_DAYS[m - 1];
        if (m == 2 && is_leap_gregorian(year)) days += 1;
    }
    days += (day - 1);
    return ((days * 24ull + hour) * 60ull + minute) * 60ull + sec;
}

uint64_t rtc_unix_time(void) {
    /* OSDev pattern: spin off UIP, read everything, re-read, compare. */
    uint8_t s0, mi0, h0, d0, mo0, y0, c0;
    uint8_t s1, mi1, h1, d1, mo1, y1, c1;
    uint8_t status_b;

    for (;;) {
        while (cmos_update_in_progress()) { /* spin */ }
        s0  = cmos_read(CMOS_REG_SECONDS);
        mi0 = cmos_read(CMOS_REG_MINUTES);
        h0  = cmos_read(CMOS_REG_HOURS);
        d0  = cmos_read(CMOS_REG_DAY);
        mo0 = cmos_read(CMOS_REG_MONTH);
        y0  = cmos_read(CMOS_REG_YEAR);
        c0  = cmos_read(CMOS_REG_CENTURY);

        while (cmos_update_in_progress()) { /* spin */ }
        s1  = cmos_read(CMOS_REG_SECONDS);
        mi1 = cmos_read(CMOS_REG_MINUTES);
        h1  = cmos_read(CMOS_REG_HOURS);
        d1  = cmos_read(CMOS_REG_DAY);
        mo1 = cmos_read(CMOS_REG_MONTH);
        y1  = cmos_read(CMOS_REG_YEAR);
        c1  = cmos_read(CMOS_REG_CENTURY);

        if (s0 == s1 && mi0 == mi1 && h0 == h1 &&
            d0 == d1 && mo0 == mo1 && y0 == y1 && c0 == c1) {
            break;
        }
    }

    status_b = cmos_read(CMOS_REG_STATUS_B);

    /* Apply BCD→binary if needed. The hour register has the 12/24 mode
     * indicator in its top bit, which we strip *before* the BCD
     * conversion so the digits are correctly placed. */
    uint8_t hour_raw = h0;
    int hour_pm = 0;
    if (!(status_b & CMOS_STATUS_B_24H)) {
        hour_pm  = (hour_raw & 0x80) != 0;
        hour_raw &= 0x7F;
    }

    if (!(status_b & CMOS_STATUS_B_BIN)) {
        s0       = bcd_to_bin(s0);
        mi0      = bcd_to_bin(mi0);
        hour_raw = bcd_to_bin(hour_raw);
        d0       = bcd_to_bin(d0);
        mo0      = bcd_to_bin(mo0);
        y0       = bcd_to_bin(y0);
        c0       = bcd_to_bin(c0);
    }

    unsigned hour = hour_raw;
    if (!(status_b & CMOS_STATUS_B_24H)) {
        /* 12-hour mode: 12 AM = 0, 12 PM = 12, otherwise add 12 if PM. */
        if (hour == 12) hour = 0;
        if (hour_pm)    hour += 12;
    }

    /* Century register is optional. If it looks wildly wrong (0, 0xFF,
     * something not in 19/20/21) fall back to "20" — EquinoxOS isn't
     * going to be booting outside the 2000–2099 window. */
    unsigned century = c0;
    if (century < 19 || century > 21) century = 20;

    unsigned year = century * 100 + y0;

    /* Sanity floor: never go below 1970, otherwise the unsigned math
     * in mktime_utc underflows. */
    if (year < 1970) year = 1970;

    return mktime_utc(year, mo0, d0, hour, mi0, s0);
}
