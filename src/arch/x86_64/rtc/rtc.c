#include "rtc.h"
#include "kernel/kprintf/kprintf.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define CMOS_SEC    0x00
#define CMOS_MIN    0x02
#define CMOS_HOUR   0x04
#define CMOS_DAY    0x07
#define CMOS_MONTH  0x08
#define CMOS_YEAR   0x09
#define CMOS_CENT   0x32
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

static int64_t boot_unix;
static int boot_year_hint = 2026;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, (uint8_t)(0x80 | reg));
    return inb(CMOS_DATA);
}

static int cmos_update_in_progress(void) {
    return (cmos_read(CMOS_STATUS_A) & 0x80) != 0;
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)(((v & 0xF0) >> 4) * 10 + (v & 0x0F));
}

struct rtc_fields {
    uint8_t sec, min, hour, day, month, year;
};

static void rtc_snapshot(struct rtc_fields *f) {
    while (cmos_update_in_progress()) { }

    f->sec = cmos_read(CMOS_SEC);
    f->min = cmos_read(CMOS_MIN);
    f->hour = cmos_read(CMOS_HOUR);
    f->day = cmos_read(CMOS_DAY);
    f->month = cmos_read(CMOS_MONTH);
    f->year = cmos_read(CMOS_YEAR);
}

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (uint64_t)d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int rtc_read_fields(struct rtc_fields *out) {
    uint8_t status_b = cmos_read(CMOS_STATUS_B);
    int binary = (status_b & 0x04) != 0;
    int hour24 = (status_b & 0x02) != 0;

    for (int tries = 0; tries < 8; tries++) {
        struct rtc_fields a, b;
        rtc_snapshot(&a);
        rtc_snapshot(&b);

        if (a.sec == b.sec && a.min == b.min && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year) {
            if (!binary) {
                a.sec = from_bcd(a.sec);
                a.min = from_bcd(a.min);
                a.hour = from_bcd(a.hour);
                a.day = from_bcd(a.day);
                a.month = from_bcd(a.month);
                a.year = from_bcd(a.year);
            }
            if (!hour24 && (a.hour & 0x80)) {
                a.hour = (uint8_t)(((a.hour & 0x7F) + 12) % 24);
            } else {
                a.hour &= 0x7F;
            }

            if (a.month < 1 || a.month > 12) return -1;
            if (a.day < 1 || a.day > 31) return -1;
            if (a.min > 59 || a.sec > 59 || a.hour > 23) return -1;

            out->sec = a.sec;
            out->min = a.min;
            out->hour = a.hour;
            out->day = a.day;
            out->month = a.month;
            out->year = a.year;
            return 0;
        }
    }
    return -1;
}

void rtc_init(void) {
    struct rtc_fields f;
    int full_year = 0;

    if (rtc_read_fields(&f) == 0) {
        int century = (f.year >= boot_year_hint % 100) ? (boot_year_hint / 100)
                                                       : ((boot_year_hint / 100) - 1);
        full_year = century * 100 + f.year;
        boot_unix = days_from_civil(full_year, f.month, f.day) * 86400
                  + (int64_t)f.hour * 3600 + (int64_t)f.min * 60 + (int64_t)f.sec;
    }

    if (full_year == 0) {
        kprintf(PRINT_SERIAL, "RTC: unreadable, unix clock starts at %lld\n",
                (unsigned long long)boot_unix);
        return;
    }

    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int wday = (int)((days_from_civil(full_year, f.month, f.day) + 4) % 7);
    if (wday < 0) wday += 7;

    kprintf(PRINT_SERIAL, "RTC: %s %04d-%02u-%02u %02u:%02u:%02u UTC (unix %lld)\n",
            wd[wday], full_year, (unsigned)f.month, (unsigned)f.day,
            (unsigned)f.hour, (unsigned)f.min, (unsigned)f.sec,
            (unsigned long long)boot_unix);
}

int64_t rtc_unix_time(void) {
    return boot_unix;
}
