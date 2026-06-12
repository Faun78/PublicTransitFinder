#ifndef QUERY_H
#define QUERY_H

#include "network.h"
#include <iostream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

enum class SearchPriority {
    LeastTransfers,
    QuickestTime
};

enum class CsvExportMode {
    TransfersOnly,
    AllStops
};

class QuerySearch;

class Query {
    friend class QuerySearch;

public:
    Query(Network& network, tm currentTime = { }, SearchPriority searchPriority = SearchPriority::QuickestTime,
            double walkingFactor = 1.3, uint32_t maxWalkingDistance = 1000, double walkingSpeed = 1.4,
            std::ostream* outStream = nullptr, bool sharedWalkingEdges = false)
        : timeInfo(currentTime)
        , network(network)
        , maxWalkingDistance(maxWalkingDistance)
        , walkingSpeed(walkingSpeed)
        , walkingFactor(walkingFactor)
        , searchPriority(searchPriority)
        , sharedWalkingEdges(sharedWalkingEdges) {
        if (isDefaultTm(timeInfo)) {
            time_t now = time(nullptr);
#ifdef _WIN32
            // Why windows, why
            localtime_s(&timeInfo, &now);
#else
            localtime_r(&now, &timeInfo);
#endif
        }
        buildWalkingIndex();
        // If caller provided an output stream, use it for direct output
        if (outStream) {
            out = outStream;
        }
    }

    // Output stream used by this Query for direct textual output (must outlive Query)
    std::ostream* getOutput() const { return out; }
    void setOutput(std::ostream* os) { out = os; }
    bool printRoute(const Path& path, int& pathCount, int maxPaths) const;
    void exportFastestArrivalCsv(std::ostream& output, CsvExportMode mode = CsvExportMode::TransfersOnly, size_t pathIndex = 0) const;
    void exportFastestArrivalCsv(const std::string& filePath, CsvExportMode mode = CsvExportMode::TransfersOnly, size_t pathIndex = 0) const;

    // This template behemoth cheks if the lookup args ar strings or locations and calls the appropriate lookup functions
    // It allows the user to call lookUp with station names, GTFS stop ids or Location without manuall creation of every combination
    template <typename T, typename Y>
    void lookUp(const T& start, const Y& end, const int maxPaths = 3) {
        constexpr bool startIsString = std::is_same_v<std::decay_t<T>, std::string>
                || std::is_array_v<T>
                || (std::is_pointer_v<std::decay_t<T>> && std::is_same_v<std::remove_pointer_t<std::decay_t<T>>, const char>);

        constexpr bool endIsString = std::is_same_v<std::decay_t<Y>, std::string>
                || std::is_array_v<Y>
                || (std::is_pointer_v<std::decay_t<Y>> && std::is_same_v<std::remove_pointer_t<std::decay_t<Y>>, const char>);

        uint32_t startId = 0, endId = 0;

        if constexpr (startIsString) {
            startId = network.lookupStationByName(std::string(start));
            if (!startId)
                startId = network.lookupStationByGtfsId(std::string(start));
        } else {
            startId = network.getStationByLocation(start);
        }

        if constexpr (endIsString) {
            endId = network.lookupStationByName(std::string(end));
            if (!endId)
                endId = network.lookupStationByGtfsId(std::string(end));
        } else {
            endId = network.getStationByLocation(end);
        }

        network.getLogger().info("Looking up path from " + std::to_string(startId) + " to " + std::to_string(endId) + " at " + formatTime(getLookupTime()));

        tm searchTimeInfo = timeInfo;

        if constexpr (!startIsString) {
            Location startLoc = network.getStationLocation(startId);
            double dist = calcDistance(startLoc, start);
            double walkingTime = dist / walkingSpeed;
            if (walkingTime > 60) {
                searchTimeInfo.tm_sec += static_cast<int>(walkingTime);

                searchTimeInfo.tm_isdst = -1;
                std::mktime(&searchTimeInfo);

                timeInfo = searchTimeInfo;
            }
        }

        findPathsByStationIds(startId, endId, maxPaths);

        if constexpr (!startIsString) {
            timeInfo = searchTimeInfo;
            timeInfo.tm_sec -= static_cast<int>(calcDistance(network.getStationLocation(startId), start) / walkingSpeed);
            std::mktime(&timeInfo);
        }

        if (!paths.empty()) {
            if constexpr (!startIsString) {
                Location startLoc = network.getStationLocation(startId);
                addWalkingToPathStart(startLoc);
            }
            if constexpr (!endIsString) {
                Location endLoc = network.getStationLocation(endId);
                addWalkingToPathEnd(endLoc);
            }
        }
    }

    const Paths& getPaths() const { return paths; }
    Paths takePaths() { return std::move(paths); }

    uint32_t getLookupTime() const { return timeInfo.tm_hour * 3600 + timeInfo.tm_min * 60 + timeInfo.tm_sec; }
    uint32_t getLookupDate() const { return (1900 + timeInfo.tm_year) * 10000 + (timeInfo.tm_mon + 1) * 100 + timeInfo.tm_mday; }
    tm getTimeInfo() const { return timeInfo; }
    uint32_t getDefaultTransferTime() const { return defaultTransferTime; }
    uint32_t getDefaultWaitingTime() const { return defaultWaitingTime; }
    uint8_t getMaxTransfers() const { return maxTransfers; }
    SearchPriority getSearchPriority() const { return searchPriority; }
    double getWalkingFactor() const { return walkingFactor; }
    uint32_t getMaxWalkingDistance() const { return maxWalkingDistance; }
    double getWalkingSpeed() const { return walkingSpeed; }

    // Setters for search parameters
    void setDefaultTransferTime(uint32_t time) { defaultTransferTime = time; }
    void setDefaultWaitingTime(uint32_t time) { defaultWaitingTime = time; }
    void setMaxTransfers(uint8_t transfers) { maxTransfers = transfers; }
    void setSearchPriority(SearchPriority priority) { searchPriority = priority; }
    void setTimeInfo(const tm& t) { timeInfo = t; }

    void setWalkingFactor(double factor) {
        walkingFactor = factor;
        buildWalkingIndex();
    }
    void setMaxWalkingDistance(uint32_t distance) {
        maxWalkingDistance = distance;
        buildWalkingIndex();
    }
    void setWalkingSpeed(double speed) {
        walkingSpeed = speed;
        buildWalkingIndex();
    }

private:
    // Lookup parameters and constants
    static constexpr uint32_t lookupSearchHorizonSeconds = 24u * 3600u;
    static constexpr int32_t transitScanWindowSeconds = 1200;
    static constexpr int maxExpansions = 500000;
    static constexpr double degToMeters = 111320.0;
    static constexpr double degToRadians = 0.017453292519943295;

    tm timeInfo; // Structured time info for the lookup
    Paths paths; // Paths found by the query
    Network& network;
    std::ostream* out = nullptr;

    uint32_t defaultTransferTime = 120; // in seconds
    uint32_t defaultWaitingTime = 1200; // How long ahead to schedule wait steps in search expansions
    uint8_t maxTransfers = 25;
    uint32_t maxWalkingDistance = 1000; // in meters
    double walkingSpeed = 1.4; // in m/s
    double walkingFactor = 1.3; // Haversine multiplier (1.0 = straight line, 1.3 = realistic streets)
    SearchPriority searchPriority = SearchPriority::LeastTransfers;
    bool sharedWalkingEdges = false;

    std::unordered_map<uint32_t, std::vector<WalkEdge>> walkingIndex;

    double getDegToMeters() const { return degToMeters; }
    double getDegToRadians() const { return degToRadians; }
    uint32_t getLookupSearchHorizonSeconds() const { return lookupSearchHorizonSeconds; }
    int32_t getTransitScanWindowSeconds() const { return transitScanWindowSeconds; }
    int getMaxExpansions() const { return maxExpansions; }
    // If sharedWalkingEdges is true, walking edges are stored in the Network and shared among all Queries to save memory, otherwise each Query builds its own walking index
    const std::unordered_map<uint32_t, std::vector<WalkEdge>>& getWalkingIndex() const { return sharedWalkingEdges ? network.getWalkingIndex() : walkingIndex; }

    void buildWalkingIndex();
    void findPathsByStationIds(uint32_t startStationId, uint32_t endStationId, const int maxPaths = 3);
    bool exportTripSegmentCsv(std::ostream& output, const std::vector<PathLeg>& legs, size_t i, size_t j) const;
    void addWalkingToPathEnd(Location endloc);
    void addWalkingToPathStart(Location startloc);
    void printRouteHeader(std::ostream& output, int& pathCount, size_t stopCount) const;
    void printRouteWalkingLeg(std::ostream& output, const PathLeg& leg, const PathLeg& nextLeg) const;
    void printRouteTransitLeg(std::ostream& output, const PathLeg& leg, const PathLeg& nextLeg, bool isTransfer) const;
    void printRouteDestinationLeg(std::ostream& output) const;

    static bool isDefaultTm(const tm& t) {
        return t.tm_sec == 0 && t.tm_min == 0 && t.tm_hour == 0
                && t.tm_mday == 0 && t.tm_mon == 0 && t.tm_year == 0
                && t.tm_wday == 0 && t.tm_yday == 0 && t.tm_isdst == 0;
    }
    static void writeTripSegmentCsvRow(std::ostream& output, const std::string& stationName,
            uint32_t arrivalDate, uint32_t arrivalTime, const Location& location, bool transferFlag);
    static bool isTransferRow(const std::vector<PathLeg>& legs, size_t i);
    static std::string formatTime(uint32_t seconds);
    static std::string formatDelay(int32_t delaySeconds);
    static std::string formatDelayShort(int32_t delaySeconds);
    static std::string formatDate(uint32_t yyyymmdd);
    static std::string csvEscape(std::string_view value);
};

#endif
