#ifndef MHDNETWORK_H
#define MHDNETWORK_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    #ifndef CPPHTTPLIB_MBEDTLS_SUPPORT
        #define CPPHTTPLIB_OPENSSL_SUPPORT
        #warning "No TLS support defined for httplib. Define CPPHTTPLIB_OPENSSL_SUPPORT or CPPHTTPLIB_MBEDTLS_SUPPORT."
    #endif
#endif


#include "handlers.h"
#include "stations.h"
#include <algorithm>
#include <ctime>
#include <httplib.h>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using StationPtr = std::unique_ptr<Station>;
struct CalendarDateExceptions;
class GTFSLoader;

double calcDistance(const Location& a, const Location& b);

enum APIEndpoint {
    NONE,
    GOLEM,
    CUSTOM
};

struct ScheduleEntry {
    uint32_t arrival_time; // Time in seconds from midnight
    uint32_t departure_time; // Time in seconds from midnight
    trip_id trip; // Trip id
    uint32_t station_id; // Station where vehicle arrives
    int stop_sequence; // Sequence index of this stop within the trip
    uint8_t day_offset; // 0 = same service day, 1 = next day - midnight wrap

    // For sorting by arrival time
    bool operator<(const ScheduleEntry& other) const {
        uint32_t a = day_offset * DateTimeUtils::DAYTIMEu + arrival_time;
        uint32_t b = other.day_offset * DateTimeUtils::DAYTIMEu + other.arrival_time;
        return a < b;
    }
};

// StopKey and TripStopIndex for fast trip+station -> schedule index lookup
struct StopKey {
    trip_id trip;
    uint32_t station_id;
    bool operator==(const StopKey& o) const {
        return trip == o.trip && station_id == o.station_id;
    }
};
// Fibonacci hashing for StopKey
struct StopKeyHash {
    size_t operator()(const StopKey& k) const noexcept {
        size_t h = std::hash<uint64_t> { }(k.trip.id);
        h ^= std::hash<uint32_t> { }(k.station_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

using TripStopIndex = std::unordered_map<StopKey, size_t, StopKeyHash>;

struct PathLeg {
    uint32_t stationId = 0;
    uint32_t elapsedArrival = 0; // schedule arrival in seconds from search start
    uint32_t elapsedDeparture = 0; // schedule departure in seconds from search start
    uint32_t line = kNoLine; // 0 = walking, else line id
    trip_id trip = trip_id(); // trip identifier for transit legs
    int32_t delaySeconds = 0; // realtime delay /s if 0 is unknown
    bool isWalk = false;
    uint32_t walkSeconds = 0; // walk duration (0 if transit leg)
};

class Path {
private:
    std::vector<PathLeg> legs;

public:
    Path() = default;
    ~Path() = default;

    void addLeg(const PathLeg& leg) {
        legs.push_back(leg);
    }
    const std::vector<PathLeg>& getLegs() const { return legs; }
    
    const int32_t getTotalTravelTime() const {
        if (legs.empty()) return 0;
        int32_t initTime = static_cast<int32_t>(legs.front().elapsedDeparture);
        int32_t finalTime = static_cast<int32_t>(legs.back().elapsedArrival);
        return finalTime - initTime;
    }

    time_t getArrivalEpoch(uint32_t baseQueryTimeSec, uint32_t baseLookupDateYYMMDD) const {
        if (legs.empty()) return 0;
        
        const PathLeg& destLeg = legs.back();
        
        uint64_t totalAbsSeconds = static_cast<uint64_t>(baseQueryTimeSec) + destLeg.elapsedArrival;
        
        if (destLeg.delaySeconds != 0) {
            totalAbsSeconds += destLeg.delaySeconds;
        }

        uint32_t daysOffset = static_cast<uint32_t>(totalAbsSeconds / DateTimeUtils::DAYTIME);
        uint32_t timeOfDaySec = static_cast<uint32_t>(totalAbsSeconds % DateTimeUtils::DAYTIME);
        uint32_t actualDate = DateTimeUtils::advanceDate(baseLookupDateYYMMDD, static_cast<int>(daysOffset));
        
        std::tm t = {};
        t.tm_year = (actualDate / 10000) - 1900;
        t.tm_mon = ((actualDate / 100) % 100) - 1;
        t.tm_mday = actualDate % 100;
        
        // Add the seconds from midnight
        t.tm_hour = 0;
        t.tm_min = 0;
        t.tm_sec = timeOfDaySec; 
        t.tm_isdst = -1; // Let the system determine daylight saving time automatically
        
        return mktime(&t);
    }
};

using Paths = std::vector<Path>;

struct WalkEdge {
    uint32_t station;
    uint32_t seconds;
};

class Network {
    friend class GTFSLoader;

public:
    struct ServiceInfo {
        uint32_t start_date = 0; // YYYYMMDD
        uint32_t end_date = 0; // YYYYMMDD
        uint8_t weekdays = 0; // bitmask Mon=1<<0 .. Sun=1<<6
    };

    Network(const std::string name = "MHD_NET", const bool onlineMode = false, Logger::Level logLevel = Logger::Level::NONE);
    ~Network();

    void loadEnvFile();
    bool loadFromAPI(APIEndpoint endpoint, const std::string& gtfsDir = "GTFS");
    bool loadFromGTFS(const std::string& gtfsDir);
    void fetchRealtimeDelays();
    void buildPlatformParentVec();
    void copyWalkingIndexFromQuery(const std::unordered_map<uint32_t, std::vector<WalkEdge>>& walkingIndex) {
        this->walkingIndex = walkingIndex;
    }

    void addStation(StationPtr& station) {
        uint32_t id = station->getId();
        stations[id] = std::move(station);
        stationsByLocation.emplace_back(id, stations[id]->getLocation());
    }

    void sortStationsByLocation() {
        std::sort(stationsByLocation.begin(), stationsByLocation.end(), [](const std::pair<uint32_t, Location>& a, const std::pair<uint32_t, Location>& b) {
            if (a.second.latitude != b.second.latitude)
                return a.second.latitude < b.second.latitude;
            return a.second.longitude < b.second.longitude;
        });
    }

    Paths findShortestPaths(uint32_t startStationId, uint32_t endStationId, uint32_t startTime = 0, uint32_t endTime = 0);
    bool isTripActiveOnDate(const trip_id& trip, uint32_t yyyymmdd) const;

    void registerGtfsStop(const std::string& gtfsId, uint32_t stationId) { gtfsStopToStation[gtfsId] = stationId; }
    void registerPlatformToParent(uint32_t platformId, uint32_t parentStationId) {
        platformToParentStation[platformId] = parentStationId;
    }

    size_t getStationCount() const { return stations.size(); }
    size_t getTripCount() const { return tripServiceInfo.size(); }
    size_t getServiceCount() const { return serviceInfoVec.size(); }
    size_t getLineCount() const { return lineSchedules.size() > 0 ? lineSchedules.size() - 1 : 0; }
    size_t getLineScheduleCount() const { return lineSchedules.size(); }
    bool isOnlineMode() const { return onlineMode; }

    uint32_t getMaxStationId() const {
        uint32_t maxId = 0;
        for (const auto& [id, _] : stations)
            if (id > maxId)
                maxId = id;
        return maxId;
    }

    uint32_t lookupStationByGtfsId(const std::string& gtfsId) const {
        auto it = gtfsStopToStation.find(gtfsId);
        return (it != gtfsStopToStation.end()) ? it->second : 0;
    }

    uint32_t getParentStation(uint32_t stationId) const {
        if (!platformParentVec.empty() && stationId < platformParentVec.size())
            return platformParentVec[stationId];
        auto it = platformToParentStation.find(stationId);
        return (it != platformToParentStation.end()) ? it->second : stationId;
    }

    int32_t getRealtimeDelay(const trip_id& trip) const {
        if (api != APIEndpoint::GOLEM)
            return 0;
        auto it = realtimeDelays.find(trip.id);
        return (it != realtimeDelays.end()) ? it->second : 0;
    }

    uint32_t lookupStationByName(const std::string& name) const;
    uint32_t getStationByLocation(const Location& location) const;
    std::string getLineName(line_id lid) const {
        if (lid == kNoLine || lid >= lineNameVec.size())
            return std::to_string(lid);
        return lineNameVec[lid];
    }

    std::string getStationName(uint32_t stationId) const {
        auto it = stations.find(stationId);
        return (it != stations.end() && it->second) ? it->second->getName() : "Unknown";
    }

    service_id getTripService(const uint64_t& tripHash) const {
        auto it2 = tripServiceInfo.find(tripHash);
        return (it2 != tripServiceInfo.end()) ? it2->second : kNoService;
    }

    const std::vector<std::pair<uint32_t, Location>>& getStationsByLocation() const { return stationsByLocation; }
    const Location& getStationLocation(uint32_t stationId) const { return stations.at(stationId)->getLocation(); }
    const std::vector<line_id>& getLinesServingStation(const uint32_t& stationId) const { return stations.at(stationId)->lines; }
    const std::vector<ScheduleEntry>& getLineSchedule(const line_id& lid) const { return lineSchedules[lid]; }
    const std::vector<TripStopIndex>& getTripStopIndex() const { return tripStopIndex; }
    const std::unordered_map<uint32_t, std::vector<WalkEdge>>& getWalkingIndex() const { return walkingIndex; }
    const std::vector<std::vector<size_t>>& getNextTripScheduleIndex() const { return nextTripScheduleIndex; }
    const std::vector<std::unordered_map<uint32_t, std::vector<size_t>>>& getStationIndex() const { return stationIndex; }
    const ServiceInfo& getServiceInfo(const service_id sid) const { return serviceInfoVec[sid]; }

    std::vector<std::string> getStationNames() const {
        std::vector<std::string> stationNames;
        for (const auto& [id, station] : stations) {
            if (station)
                stationNames.push_back(station->getName());
        }
        return stationNames;
    }

    void setOnlineMode(bool online) { onlineMode = online; }
    Logger& getLogger() { return logger; }
    const Logger& getLogger() const { return logger; }

    std::vector<std::vector<ScheduleEntry>>& getLineSchedules() { return lineSchedules; }
    std::vector<std::string>& getLineNameVec() { return lineNameVec; }
    std::vector<ServiceInfo>& getServiceInfoVec() { return serviceInfoVec; }
    std::unordered_map<uint64_t, service_id>& getTripServiceInfo() { return tripServiceInfo; }
    std::vector<TripStopIndex>& getTripStopIndex() { return tripStopIndex; }
    std::vector<std::unordered_map<uint32_t, std::vector<size_t>>>& getStationIndex() { return stationIndex; }
    std::vector<std::vector<size_t>>& getNextTripScheduleIndex() { return nextTripScheduleIndex; }
    std::unordered_map<uint64_t, int32_t>& getRealtimeDelays() { return realtimeDelays; }
    std::map<uint32_t, StationPtr>& getStationsMap() { return stations; }
    std::unique_ptr<CalendarDateExceptions>& getCalendarExceptions() { return calendarExceptions; }
    APIEndpoint& getApiEndpoint() { return api; }
    bool getOnlineMode() { return onlineMode; }

private:
    bool onlineMode;
    APIEndpoint api = APIEndpoint::NONE;
    Logger logger;

    // Station ID -> Station
    std::map<uint32_t, StationPtr> stations;
    // Sorted vector of station ids by location
    std::vector<std::pair<uint32_t, Location>> stationsByLocation;
    // line_id -> schedule
    std::vector<std::vector<ScheduleEntry>> lineSchedules;
    // line_id -> readable name
    std::vector<std::string> lineNameVec;
    // service_id -> ServiceInfo
    std::vector<ServiceInfo> serviceInfoVec;

    // trip_id.id (hash) -> service_id
    std::unordered_map<uint64_t, service_id> tripServiceInfo;
    // Fast trip+station -> schedule index lookup
    std::vector<TripStopIndex> tripStopIndex;
    // Fast schedule index -> next stop index for the same trip
    std::vector<std::vector<size_t>> nextTripScheduleIndex;
    // station_id -> vector of (line_id, schedule index) for all lines serving that station
    std::vector<std::unordered_map<uint32_t, std::vector<size_t>>> stationIndex;
    // Map each platform (GTFS stop_id) to its parent station
    std::unordered_map<uint32_t, uint32_t> platformToParentStation;
    // flat vector mapping platformId -> parentStationId
    std::vector<uint32_t> platformParentVec;
    // GTFS stop_id -> internal station id
    std::map<std::string, uint32_t> gtfsStopToStation;

    // Calendar date exceptions: date (YYYYMMDD) -> set of service_ids removed on that date
    std::unique_ptr<CalendarDateExceptions> calendarExceptions;
    // Real-time delays: trip_id.id -> delay in seconds
    std::unordered_map<uint64_t, int32_t> realtimeDelays;

    // Walking graph: station_id -> vector of (neighbor station_id, walk time seconds)
    // Used only when MHDCore inits the network in multi-core mode
    std::unordered_map<uint32_t, std::vector<WalkEdge>> walkingIndex;
};

#endif
