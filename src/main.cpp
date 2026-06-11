#include "mhd_core.h"
#include <ctime>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::string gtfsDir = (argc > 1) ? argv[1] : "PID_GTFS";
    std::string dateArg;
    std::string timeArg;
    if (argc > 3) {
        dateArg = argv[2];
        timeArg = argv[3];
    } else if (argc > 2) {
        timeArg = argv[2];
    }
    bool onlineMode = true;
    MHDCore core("MHD_NET", onlineMode, Logger::Level::INFO);
    if (!core.loadGTFS(gtfsDir, APIEndpoint::CUSTOM)) {
        return 2;
    }
    std::cout << "Stations: " << core.getStationCount() << "\n";
    std::cout << "Trips: " << core.getTripCount() << "\n";
    std::cout << "Lines: " << core.getLineCount() << "\n";
    tm currentTime = { };
    std::time_t now = std::time(nullptr);
    std::tm* now_tm = std::localtime(&now);
    if (now_tm)
        currentTime = *now_tm;
    if (!dateArg.empty()) {
        int year = 0;
        int month = 0;
        int day = 0;
        if (sscanf(dateArg.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
            currentTime.tm_year = year - 1900;
            currentTime.tm_mon = month - 1;
            currentTime.tm_mday = day;
        }
    }
    if (!timeArg.empty()) {
        int hour = 0;
        int minute = 0;
        if (sscanf(timeArg.c_str(), "%d:%d", &hour, &minute) == 2) {
            currentTime.tm_hour = hour;
            currentTime.tm_min = minute;
            currentTime.tm_sec = 0;
            currentTime.tm_isdst = -1;
        }
    }

    mktime(&currentTime);
    int qid = core.newQuery(currentTime, SearchPriority::QuickestTime, 1.3, 1000, 1.4);
    auto res = core.lookUpArrival(qid, "Pelc Tyrolka", "Dejvická");
    core.printPaths(qid, std::cout);
    return 0;
}
