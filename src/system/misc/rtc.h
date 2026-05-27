#ifndef RTC_H
#define RTC_H
/* ---------------------------------------------------------------------------
 * CMOS / RTC wall-clock reader.
 *
 * The MC146818-compatible CMOS RTC is wired to legacy I/O ports 0x70 (index)
 * and 0x71 (data). It survives reboot via the motherboard battery and is
 * what BIOS/QEMU use as the persistent date+time. We read it directly
 * because EquinoxOS doesn't have NTP, ACPI tables, or hpet bring-up yet —
 * and for what TLS needs (a roughly-accurate UTC for X.509 NotBefore/After
 * checks) the CMOS clock is more than enough.
 *
 * Two formats can be in play at runtime:
 *
 *   * Status register B bit 2 == 1 ⇒ values in CMOS registers are stored
 *     in plain binary. Modern firmware (incl. QEMU) usually sets this.
 *   * Bit 2 == 0 ⇒ BCD; each nibble holds a decimal digit.
 *
 *   * Status register B bit 1 == 1 ⇒ 24-hour mode.
 *   * Bit 1 == 0 ⇒ 12-hour mode with the top bit of the hour register
 *     meaning PM. Painful but real BIOSes still ship it.
 *
 * The RTC ticks once per second. To avoid reading a half-updated value we
 * wait for status register A bit 7 (UIP, Update-In-Progress) to clear,
 * then read everything atomically, then re-read and compare — if anything
 * differs we retry. This is the standard OSDev pattern.
 *
 * The 21st-century century byte is at register 0x32 on most boards (and on
 * QEMU's piix4); we fall back to "20" if the read looks bogus, since
 * EquinoxOS is unlikely to be running pre-2000 or post-2099.
 *
 * Returns:
 *   sys_rtc_unix_time() — 64-bit Unix timestamp in *whole seconds* (UTC).
 *                         Never fails in the API sense; if the hardware
 *                         is wedged the function still returns a value,
 *                         but BR_ERR_X509_NOT_TRUSTED on first TLS
 *                         attempt will make that obvious. */

#include <stdint.h>

/* Read the current wall-clock time from the CMOS RTC and return it as
 * seconds since 1970-01-01 00:00:00 UTC. Assumes the RTC is configured
 * for UTC, which is the QEMU default. On real hardware where the user
 * has the BIOS set to "Local time", values will be off by their TZ
 * offset — fix later by adding an explicit /etc/equos/tz file. */
uint64_t rtc_unix_time(void);

#endif /* RTC_H */
