#ifndef MHD_CORE_H
#define MHD_CORE_H

#include "query.h"
#include <ostream>

class MHDCore {
private:
    Network network;
    std::vector<Query> querries;
    std::string GTFSDir = "GTFS_DATA";
    APIEndpoint apiEndpoint = APIEndpoint::NONE;

    static std::string formatTime(time_t time) {
        char buf[9];
        strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&time));
        return std::string(buf);
    }

public:
    struct LookupResult {
        Paths paths;
        std::string status;
    };
    MHDCore(const std::string& networkName, bool onlineMode = false, Logger::Level logLevel = Logger::Level::NONE, std::ostream* loggerStream = &std::cerr)
        : network(networkName, onlineMode) {
        network.getLogger().setLevel(logLevel);
        if (loggerStream) {
            network.getLogger().setOutput(*loggerStream);
        }
    }
    bool loadGTFS(const std::string& gtfsDir, APIEndpoint endpoint);
    // Load GTFS directly from a local GTFS directory
    bool loadFromGTFS(const std::string& gtfsDir) { return network.loadFromGTFS(gtfsDir); }
    size_t getStationCount() const { return network.getStationCount(); }
    size_t getTripCount() const { return network.getTripCount(); }
    size_t getLineCount() const { return network.getLineCount(); }
    int newQuery(tm currentTime = { }, SearchPriority searchPriority = SearchPriority::QuickestTime,
            double walkingFactor = 1.3, uint32_t maxWalkingDistance = 700, double walkingSpeed = 1.4, int maxTransfers = 25,
            std::ostream* outStream = nullptr);
    template <typename T, typename Y>
    LookupResult lookUp(const int queryID, const T& start, const Y& end, const int maxPaths = 3, tm timeInfo = { }) {
        if (!isValidQueryID(queryID)) {
            return { { }, "Invalid query ID" };
        }
        Query& q = querries[queryID];
        bool isDefault = timeInfo.tm_sec == 0 && timeInfo.tm_min == 0 && timeInfo.tm_hour == 0
                && timeInfo.tm_mday == 0 && timeInfo.tm_mon == 0 && timeInfo.tm_year == 0
                && timeInfo.tm_wday == 0 && timeInfo.tm_yday == 0 && timeInfo.tm_isdst == 0;
        if (!isDefault) {
            q.setTimeInfo(timeInfo);
        }
        q.lookUp(start, end, maxPaths);
        if (q.getPaths().empty()) {
            return { Paths { }, "No paths found" };
        }
        return { q.getPaths(), "OK" };
    }
    template <typename T, typename Y>
    LookupResult lookUpArrival(const int queryID, const T& start, const Y& end, const int maxPaths = 3, tm timeInfo = { }) {
        if (!isValidQueryID(queryID)) {
            return { { }, "Invalid query ID" };
        }
        Query& q = querries[queryID];

        bool isDefault = timeInfo.tm_sec == 0 && timeInfo.tm_min == 0 && timeInfo.tm_hour == 0
                && timeInfo.tm_mday == 0 && timeInfo.tm_mon == 0 && timeInfo.tm_year == 0
                && timeInfo.tm_wday == 0 && timeInfo.tm_yday == 0 && timeInfo.tm_isdst == 0;
        if (!isDefault) {
            q.setTimeInfo(timeInfo);
        }

        tm originalArrivalTime = q.getTimeInfo();
        time_t targetArrivalEpoch = mktime(&originalArrivalTime);

        const time_t WINDOW_SIZE = 4 * 3600;
        const int MAX_ATTEMPTS = 6; // Dynamically expands backward across long distances if needed

        time_t currentWindowRight = targetArrivalEpoch;
        time_t bestDepartureEpoch = 0;
        bool foundValidRoute = false;

        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            time_t left = currentWindowRight - WINDOW_SIZE;
            time_t right = currentWindowRight;

            // Track the last processed minute value to eliminate duplicate scans
            time_t lastSeenMinuteEpoch = 0;

            // Binary Search Loop
            while (left <= right) {
                if (right - left < 60) {
                    break;
                }

                time_t mid = left + (right - left) / 2;

                time_t midNormalizedToMinute = (mid / 60) * 60;

                if (midNormalizedToMinute == lastSeenMinuteEpoch) {
                    break;
                }
                lastSeenMinuteEpoch = midNormalizedToMinute;

                std::tm midTime = {}; 
                localtime_r(&midNormalizedToMinute, &midTime);

                q.setTimeInfo(midTime);
                q.lookUp(start, end, 1);

                if (q.getPaths().empty()) {
                    right = midNormalizedToMinute - 60; // Push back by a whole minute
                    continue;
                }

                const Path& primaryPath = q.getPaths().front();
                time_t pathArrivalEpoch = primaryPath.getArrivalEpoch();

                if (pathArrivalEpoch <= targetArrivalEpoch) {
                    bestDepartureEpoch = midNormalizedToMinute;
                    foundValidRoute = true;
                    left = midNormalizedToMinute + 60; // Try searching later
                } else {
                    right = midNormalizedToMinute - 60; // Too late, shift search window earlier
                }
            }

            if (foundValidRoute) {
                break;
            }

            // Slide the target window back if the current block was empty
            currentWindowRight -= WINDOW_SIZE;
        }

        if (foundValidRoute) {
            tm finalDepartureTime;
            localtime_r(&bestDepartureEpoch, &finalDepartureTime);
            q.setTimeInfo(finalDepartureTime);
            q.lookUp(start, end, maxPaths);
            return { q.getPaths(), "OK" };
        }

        return { Paths { }, "No paths found arriving before the requested time" };
    }
    std::vector<std::string> getStationNames() const { return network.getStationNames(); }
    void exportPathsToCsv(const int queryID, std::ostream& output, CsvExportMode mode = CsvExportMode::TransfersOnly);
    void printPaths(const int queryID, std::ostream& output);
    bool isValidQueryID(int queryID) const { return queryID >= 0 && static_cast<size_t>(queryID) < querries.size(); }
    bool refreshGTFS(const std::string& gtfsDir, APIEndpoint endpoint, bool onlineMode = false);
};

#endif