#pragma once

#include <cstdint>
#include <string>

#include "errors.h"

constexpr int DATETIME_LEN = sizeof(int64_t);

inline bool is_leap_year(int year) {
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

inline int days_in_month(int year, int month) {
    static constexpr int DAYS[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return DAYS[month];
}

inline int parse_datetime_field(const std::string &value, int offset, int length) {
    int result = 0;
    for (int i = 0; i < length; ++i) {
        char ch = value[offset + i];
        if (ch < '0' || ch > '9') {
            throw RMDBError("Invalid datetime value: " + value);
        }
        result = result * 10 + (ch - '0');
    }
    return result;
}

inline int64_t parse_datetime(const std::string &value) {
    if (value.size() != 19 || value[4] != '-' || value[7] != '-' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':') {
        throw RMDBError("Invalid datetime value: " + value);
    }

    int year = parse_datetime_field(value, 0, 4);
    int month = parse_datetime_field(value, 5, 2);
    int day = parse_datetime_field(value, 8, 2);
    int hour = parse_datetime_field(value, 11, 2);
    int minute = parse_datetime_field(value, 14, 2);
    int second = parse_datetime_field(value, 17, 2);

    if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        throw RMDBError("Invalid datetime value: " + value);
    }

    return static_cast<int64_t>(year) * 10000000000LL + static_cast<int64_t>(month) * 100000000LL +
           static_cast<int64_t>(day) * 1000000LL + static_cast<int64_t>(hour) * 10000LL +
           static_cast<int64_t>(minute) * 100LL + second;
}

inline std::string format_datetime(int64_t value) {
    std::string digits = std::to_string(value);
    if (digits.size() != 14) {
        throw RMDBError("Invalid stored datetime value");
    }
    return digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2) + " " +
           digits.substr(8, 2) + ":" + digits.substr(10, 2) + ":" + digits.substr(12, 2);
}
