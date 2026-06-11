#include "mhd_core.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

#ifndef _WIN32
// RSS memory works only on Linux as Windows needs other lib
static size_t getCurrentRSSBytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string key;
            size_t kb = 0;
            std::string unit;
            iss >> key >> kb >> unit;
            return kb * 1024;
        }
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    const int N = (argc > 1) ? std::atoi(argv[1]) : 10000;
    const std::string gtfsDir = (argc > 2) ? argv[2] : "PID_GTFS";
    SearchPriority priority = SearchPriority::QuickestTime;
    if (argc > 3) {
        std::string mode = argv[3];
        for (char& c : mode)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (mode == "least" || mode == "leasttransfers")
            priority = SearchPriority::LeastTransfers;
        else if (mode == "quickest" || mode == "fastest" || mode == "quickesttime")
            priority = SearchPriority::QuickestTime;
        else {
            std::cerr << "Unknown priority mode: " << argv[3] << " (use least or quickest)\n";
            return 2;
        }
    }

    MHDCore core("MHD_TEST", false, Logger::Level::NONE);
    try {
        core.loadFromGTFS(gtfsDir);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load GTFS: " << e.what() << std::endl;
        return 2;
    }

    auto stationNames = core.getStationNames();
    if (stationNames.size() < 2) {
        std::cerr << "Not enough stations to run test" << std::endl;
        return 2;
    }
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<size_t> distStation(0, stationNames.size() - 1);
    std::uniform_int_distribution<uint32_t> distTime(0, 24u * 3600u - 1);
    std::time_t now = std::time(nullptr);
    std::tm tm_lookup;
#ifdef _WIN32
    localtime_s(&tm_lookup, &now);
#else
    localtime_r(&now, &tm_lookup);
#endif
    int qid = core.newQuery(tm_lookup, priority, 1.3, 1000, 1.4, 10, nullptr);
#ifndef _WIN32
    size_t maxRSS = getCurrentRSSBytes();
#endif
    auto t0 = std::chrono::steady_clock::now();

    int successfulLookups = 0;
    int failedLookups = 0;
    tm today = tm_lookup;
    mktime(&today);

    const int reportInterval = std::max(1, N / 20);
    for (int i = 0; i < N; ++i) {
        size_t si = distStation(rng);
        size_t ei = distStation(rng);
        while (ei == si)
            ei = distStation(rng);
        const std::string& sname = stationNames[si];
        const std::string& ename = stationNames[ei];
        uint32_t t = distTime(rng);

        tm_lookup.tm_hour = static_cast<int>(t / 3600u);
        tm_lookup.tm_min = static_cast<int>((t % 3600u) / 60u);
        tm_lookup.tm_sec = static_cast<int>(t % 60u);
        tm_lookup.tm_mday = distTime(rng) % 28 + 1;
        tm_lookup.tm_mday = std::max(today.tm_mday, tm_lookup.tm_mday);
        mktime(&tm_lookup);

        auto res = core.lookUp(qid, sname, ename, 2, tm_lookup);
        const auto& paths = res.paths;
        if (!paths.empty()) {
            ++successfulLookups;
        } else {
            ++failedLookups;
        }

        if (i % reportInterval == 0) {
#ifndef _WIN32
            size_t rss = getCurrentRSSBytes();
            if (rss > maxRSS)
                maxRSS = rss;
            std::cerr << "iter=" << i << " rssKB=" << (rss / 1024) << " paths=" << paths.size() << std::endl;
#else
            std::cerr << "iter=" << i << " paths=" << paths.size() << std::endl;
#endif
            std::cerr << "  " << successfulLookups << " successful lookups, " << failedLookups << " failed lookups" << std::endl;
            std::cerr << "Success rate: " << (100.0 * successfulLookups / (double)(successfulLookups + failedLookups)) << "%" << std::endl;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> dur = t1 - t0;
#ifndef _WIN32
    size_t finalRSS = getCurrentRSSBytes();
#endif
    std::cout << "Ran " << N << " searches in " << dur.count() << "s using "
              << (priority == SearchPriority::LeastTransfers ? "LeastTransfers" : "QuickestTime") << "\n";
#ifndef _WIN32
    std::cout << "Peak RSS: " << (maxRSS / 1024) << " KiB\n";
    std::cout << "Final RSS: " << (finalRSS / 1024) << " KiB\n";
#endif
    std::cout << "Successful lookups: " << successfulLookups << "\n";
    std::cout << "Failed lookups: " << failedLookups << "\n";

    return 0;
}
