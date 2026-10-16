#ifndef RTC_H
#define RTC_H

#include <stdint.h>

void rtc_init(void);
int64_t rtc_unix_time(void);

#endif