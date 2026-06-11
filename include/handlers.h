#ifndef MHD_HANDLERS_H
#define MHD_HANDLERS_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

class MHDException : public std::runtime_error {
public:
    explicit MHDException(const std::string& msg)
        : std::runtime_error(msg) { }
    explicit MHDException(const char* msg)
        : std::runtime_error(msg) { }
};

class DateTimeUtils {
public:
    constexpr static int32_t DAYTIME = (24 * 3600);
    constexpr static uint32_t DAYTIMEu = (24u * 3600u);
    static constexpr int convertTimeToSeconds(std::string_view timeStr) {
        if (timeStr.size() != 8 || timeStr[2] != ':' || timeStr[5] != ':')
            return -1;
        int hours = (timeStr[0] - '0') * 10 + (timeStr[1] - '0');
        int minutes = (timeStr[3] - '0') * 10 + (timeStr[4] - '0');
        int seconds = (timeStr[6] - '0') * 10 + (timeStr[7] - '0');
        return hours * 3600 + minutes * 60 + seconds;
    }

    static uint32_t parseDateYYYYMMDD(std::string_view s) {
        if (s.size() != 8)
            return 0;
        uint32_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9')
                return 0;
            v = v * 10 + static_cast<uint32_t>(c - '0');
        }
        return v;
    }

    static int weekdayFromYYYYMMDD(uint32_t value) {
        if (value == 0)
            return 0;
        int y = static_cast<int>(value / 10000);
        int m = static_cast<int>((value / 100) % 100);
        int d = static_cast<int>(value % 100);
        // https://www.geeksforgeeks.org/dsa/zellers-congruence-find-day-date/
        static int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
        if (m < 3)
            y -= 1;
        int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
        if (w == 0)
            return 6;
        return w - 1;
    }

    static uint32_t advanceDate(uint32_t yyyymmdd, int days) noexcept {
        if (days == 0)
            return yyyymmdd;
        int y = static_cast<int>(yyyymmdd / 10000);
        int m = static_cast<int>((yyyymmdd / 100) % 100);
        int d = static_cast<int>(yyyymmdd % 100) + days;
        auto isLeap = [](int yr) { return (yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0); };
        const int dim[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        while (d > dim[m] + (m == 2 && isLeap(y) ? 1 : 0)) {
            d -= dim[m] + (m == 2 && isLeap(y) ? 1 : 0);
            if (++m > 12) {
                m = 1;
                y++;
            }
        }
        while (d < 1) {
            if (--m < 1) {
                m = 12;
                y--;
            }
            d += dim[m] + (m == 2 && isLeap(y) ? 1 : 0);
        }
        return static_cast<uint32_t>(y * 10000 + m * 100 + d);
    }

    static uint32_t toAbsSeconds(uint8_t baseDay, uint8_t day, uint32_t time) {
        return static_cast<uint8_t>((day - baseDay + 7) % 7) * DAYTIMEu + time;
    }
};

// Windows headers define these as macros
#ifdef _WIN32
    #ifdef ERROR
        #undef ERROR
    #endif
    #ifdef INFO
        #undef INFO
    #endif
    #ifdef WARNING
        #undef WARNING
    #endif
    #ifdef CRITICAL
        #undef CRITICAL
    #endif
    #ifdef NONE
        #undef NONE
    #endif
#endif

class Logger {
public:
    enum class Level {
        NONE = 0, // only critical
        ERROR = 1, // error + critical
        INFO = 2 // info + error + critical
    };
    // Add name to the logger
    static void setName(std::string_view name, bool loadLevelFromEnv = true) {
        std::lock_guard<std::recursive_mutex> lg(mu());
        loggerName() = std::string(name);
        if (loadLevelFromEnv) {
            level() = initLevelFromEnv();
        }
    }
    static void setName(const std::string& name, Level _level) {
        std::lock_guard<std::recursive_mutex> lg(mu());
        loggerName() = name;
        level() = _level;
    }

    static std::string getName() {
        std::lock_guard<std::recursive_mutex> lg(mu());
        return loggerName();
    }

    // Set the global output stream (must outlive logging calls)
    static void setOutput(std::ostream& os) {
        std::lock_guard<std::recursive_mutex> lg(mu());
        out() = &os;
    }

    static void setLevel(Level l) {
        std::lock_guard<std::recursive_mutex> lg(mu());
        level() = l;
    }

    static Level getLevel() {
        std::lock_guard<std::recursive_mutex> lg(mu());
        return level();
    }

    static void info(std::string_view s) {
        if (getLevel() >= Level::INFO)
            log(getName().empty() ? "-INFO: " : (getName() + "-INFO: ").c_str(), s);
    }

    static void error(std::string_view s) {
        if (getLevel() >= Level::ERROR)
            log(getName().empty() ? "-ERROR: " : (getName() + "-ERROR: ").c_str(), s);
    }

    static void critical(std::string_view s) {
        // critical messages always shown
        log(getName().empty() ? "-CRITICAL: " : (getName() + "-CRITICAL: ").c_str(), s);
    }

private:
    static void log(const char* prefix, std::string_view s) {
        std::lock_guard<std::recursive_mutex> lg(mu());
        if (out())
            (*out()) << prefix << s << '\n';
    }

    static std::ostream*& out() {
        static std::ostream* inst = &std::cerr;
        return inst;
    }

    static std::recursive_mutex& mu() {
        static std::recursive_mutex m;
        return m;
    }
    // Add default logger name
    static std::string& loggerName() {
        static std::string name = "Logger";
        return name;
    }

    // initialize level from environment variable MHD_LOG_LEVEL (NONE, ERROR, INFO)
    static Level initLevelFromEnv() {
        const char* e = std::getenv((getName() + "_LOG_LEVEL").c_str());
        if (!e)
            return Level::NONE;
        std::string s(e);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        if (s == "INFO")
            return Level::INFO;
        if (s == "ERROR")
            return Level::ERROR;
        return Level::NONE;
    }

    static Level& level() {
        static Level l = initLevelFromEnv();
        return l;
    }
};

#endif
