/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <cstdint>
#include "rtc.hh"
#include "processor.hh"

#define RTC_PORT(x) (0x70 + (x))
#define RTC_BINARY_DATE 0x4

rtc::rtc()
{
    auto status = cmos_read(0xB);
    _is_bcd = !(status & RTC_BINARY_DATE);
}

uint8_t rtc::cmos_read(uint8_t addr)
{
    processor::outb(addr, RTC_PORT(0));
    return processor::inb(RTC_PORT(1));
}

uint8_t rtc::cmos_read_date(uint8_t addr)
{
    uint8_t val = cmos_read(addr);
    if (!_is_bcd)
        return val;
    return (val & 0x0f) + (val >> 4) * 10;
}

uint64_t rtc::wallclock_ns()
{
    // 0x80 : Update in progress. Wait for it.
    while ((cmos_read(0xA) & 0x80));

    uint8_t year = cmos_read_date(9);
    uint8_t month = cmos_read_date(8);
    uint8_t day = cmos_read_date(7);
    uint8_t hours = cmos_read_date(4);
    uint8_t mins = cmos_read_date(2);
    uint8_t secs = cmos_read_date(0);

    // FIXME: Get century from FADT.
    // Convert the broken-down RTC date/time to nanoseconds since the Unix
    // epoch. Unix time ignores leap seconds, so the conversion is the standard
    // days-from-civil computation (proleptic Gregorian; Howard Hinnant's
    // algorithm) plus the time-of-day, scaled to nanoseconds.
    int y = 2000 + year;
    unsigned m = month, d = day;
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;

    const int64_t secs_since_epoch =
        days * 86400 + hours * 3600 + mins * 60 + secs;

    return static_cast<uint64_t>(secs_since_epoch) * 1000000000ull;
}
