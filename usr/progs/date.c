#include "syscall.h"
#include "string.h"
#include "shell.h"

#define DATE_FD 4

unsigned int year_days = 365;
unsigned int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
const char *month_names[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

void main(int argc, char* argv[]) {
    // date should take no args
    if (argc > 1) {
        dprintf(CONSOLEOUT, "date: too many arguments\n");
        return;
    }

    // Get time from rtc
    int result = _open(DATE_FD, "dev/rtc0");
    if (result < 0) {
        // Could not open RTC device
        dprintf(CONSOLEOUT, "date: cannot open RTC device, err code %d\n", result);
        return;
    }

    unsigned long long ns;
    int bytes = _read(DATE_FD, &ns, sizeof(ns));
    if (bytes < (int)sizeof(ns)) {
        // Fail to read from RTC
        dprintf(CONSOLEOUT, "date: error reading from RTC\n");
        _close(DATE_FD);
        return;
    }
    
    _close(DATE_FD);  // Close anyway

    // Convert time
    unsigned long long total_sec = ns / 1000000000ULL;
    unsigned long long days = total_sec / 86400ULL;
    unsigned long long sec_of_day = total_sec % 86400ULL;
    unsigned int sec = sec_of_day % 60;
    unsigned int min = (sec_of_day / 60) % 60;
    unsigned int hour = sec_of_day / 3600;
    unsigned int year = 1970;  // From 01 Jan 1970
    while (1) {
        // Calc days in the current year by subtracting year by year
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            year_days = 366;  // Leap year
        } else {
            year_days = 365;
        }
        if (days < year_days) {
            break;
        }
        days -= year_days;
        year++;
    }
    unsigned int day_of_the_year = (unsigned int) days;  // 0 = 01 Jan
    unsigned int month_index = 0;
    
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
        month_days[1] = 29;  // Leap year
    }
    while (month_index < 12 && day_of_the_year >= month_days[month_index]) {
        day_of_the_year -= month_days[month_index];
        month_index++;
    }
    unsigned int month_day = day_of_the_year + 1;

    // Format as DD Mon YYYY HH:MM:SS
    dprintf(STDOUT, "%02u %s %04u %02u:%02u:%02u\n",
        month_day, month_names[month_index], year, hour, min, sec);
}
