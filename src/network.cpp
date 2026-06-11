
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "gtfs_loader.h"
#include "handlers.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

#ifdef _WIN32
constexpr double M_PI = 3.14159265358979323846;
#endif

Network::~Network() = default;

// Haversine formula for distance between two lat/lon points
double calcDistance(const Location& a, const Location& b) {
    constexpr double kDeg2Rad = M_PI / 180.0;
    constexpr double kEarthRadius = 6371000.0;
    double lat1 = a.latitude * kDeg2Rad, lon1 = a.longitude * kDeg2Rad;
    double lat2 = b.latitude * kDeg2Rad, lon2 = b.longitude * kDeg2Rad;
    double dlat = lat2 - lat1, dlon = lon2 - lon1;
    double h = sin(dlat / 2) * sin(dlat / 2)
            + cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);
    return kEarthRadius * 2.0 * atan2(sqrt(h), sqrt(1.0 - h));
}

Network::Network(const std::string name, const bool onlineMode, Logger::Level logLevel)
    : onlineMode(onlineMode)
    , calendarExceptions(std::make_unique<CalendarDateExceptions>()) {
    lineSchedules.emplace_back();
    lineNameVec.emplace_back();
    serviceInfoVec.emplace_back();
    loadEnvFile();
    logger.setName(name, logLevel);
}

void Network::loadEnvFile() {
    static const char* kCandidates[] = { ".env", "../.env", "../../.env" };
    for (const char* path : kCandidates) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            auto trim = [](std::string& s) {
                size_t a = s.find_first_not_of(" \t");
                size_t b = s.find_last_not_of(" \t");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trim(key);
            trim(value);

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            if (!key.empty() && !value.empty()) {
#ifdef _WIN32
                _putenv_s(key.c_str(), value.c_str());
#else
                setenv(key.c_str(), value.c_str(), 1);
#endif
            }
        }
        return;
    }
}

bool Network::isTripActiveOnDate(const trip_id& trip, uint32_t yyyymmdd) const {
    auto sidIt = tripServiceInfo.find(trip.id);
    if (sidIt == tripServiceInfo.end())
        return true;
    // Firestly check for specific exceptions for the date
    service_id sid = sidIt->second;
    auto addIt = calendarExceptions->added.find(yyyymmdd);
    if (addIt != calendarExceptions->added.end() && addIt->second.count(sid))
        return true;
    auto remIt = calendarExceptions->removed.find(yyyymmdd);
    if (remIt != calendarExceptions->removed.end() && remIt->second.count(sid))
        return false;
    if (sid >= serviceInfoVec.size())
        return true;
    // original service info check - date range and weekday
    const ServiceInfo& info = serviceInfoVec[sid];
    if (info.weekdays == 0 && info.start_date == 0 && info.end_date == 0)
        return false;
    if (info.start_date != 0 && yyyymmdd < info.start_date)
        return false;
    if (info.end_date != 0 && yyyymmdd > info.end_date)
        return false;
    // fall back to weekday check if no specific date range is given
    int weekday = DateTimeUtils::weekdayFromYYYYMMDD(yyyymmdd);
    return (info.weekdays & (1 << weekday)) != 0;
}
// Solve multiple platforms with same location by linking them to a parent station
// Kolej 1 -> Hloubetin metro -> Hloubetin
void Network::buildPlatformParentVec() {
    uint32_t maxId = 0;
    for (const auto& p : stationsByLocation)
        if (p.first > maxId)
            maxId = p.first;
    platformParentVec.assign(maxId + 1, 0);
    for (uint32_t i = 0; i <= maxId; ++i)
        platformParentVec[i] = i;
    for (const auto& kv : platformToParentStation) {
        if (kv.first <= maxId)
            platformParentVec[kv.first] = kv.second;
    }
}

// Find closest station to given location, using the pre-sorted stationsByLocation
uint32_t Network::getStationByLocation(const Location& location) const {
    const auto& stationsByLoc = getStationsByLocation();
    if (stationsByLoc.empty())
        return 0;

    double minDistance = std::numeric_limits<double>::max();
    uint32_t closestStationId = 0;

    auto it = std::lower_bound(stationsByLoc.begin(), stationsByLoc.end(), location.latitude,
            [](const std::pair<uint32_t, Location>& station, double lat) {
                return station.second.latitude < lat;
            });

    int searchRadius = 100;
    auto start_it = it - std::min((int)std::distance(stationsByLoc.begin(), it), searchRadius);
    auto end_it = it + std::min((int)std::distance(it, stationsByLoc.end()), searchRadius);

    for (auto sit = start_it; sit != end_it; ++sit) {
        double distance = calcDistance(location, sit->second);
        if (distance < minDistance) {
            minDistance = distance;
            closestStationId = sit->first;
        }
    }

    return closestStationId;
}

// We lookup by making lowercase and comparing, but if not found,
// we also look for partial matches and return the best match
uint32_t Network::lookupStationByName(const std::string& name) const {
    if (name.empty())
        return 0;

    auto lower = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s)
            out.push_back(static_cast<char>(std::tolower(c)));
        return out;
    };

    const std::string target = lower(name);

    for (const auto& [id, st] : stations)
        if (st && lower(st->getName()) == target)
            return id;

    std::vector<std::pair<uint32_t, std::string>> candidates;
    for (const auto& [id, st] : stations) {
        if (!st)
            continue;
        std::string sname = lower(st->getName());
        if (sname.find(target) != std::string::npos)
            candidates.emplace_back(id, st->getName());
    }

    if (candidates.empty())
        return 0;

    std::sort(candidates.begin(), candidates.end(),
            [&lower, &target](const auto& a, const auto& b) {
                std::string al = lower(a.second), bl = lower(b.second);
                bool aPrefix = al.rfind(target, 0) == 0;
                bool bPrefix = bl.rfind(target, 0) == 0;
                if (aPrefix != bPrefix)
                    return aPrefix;
                if (al.size() != bl.size())
                    return al.size() < bl.size();
                return a.first < b.first;
            });

    return candidates.front().first;
}

void Network::fetchRealtimeDelays() {
    if (!getOnlineMode() || getApiEndpoint() != APIEndpoint::GOLEM)
        return;

    const char* keyEnv = std::getenv("GOLEM_API_KEY");
    if (!keyEnv || !*keyEnv) {
        logger.error("GOLEM_API_KEY not set -> real-time delays unavailable.");
        return;
    }
    logger.info("Using Golem API");
    std::string apiKey = keyEnv;
    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };
    trim(apiKey);
    if (apiKey.empty()) {
        logger.error("GOLEM_API_KEY is blank -> real-time delays unavailable.");
        return;
    }

    httplib::Client cli("https://api.golemio.cz");
#ifdef _WIN32
    // Windows needs more libraries and I don't want to deal with that, so just disable certificate verification on Windows
    cli.enable_server_certificate_verification(false);
#endif
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers {
        { "X-Access-Token", apiKey },
        { "Accept", "application/json" }
    };

    std::string path = "/v2/vehiclepositions"
                       "?limit=7000"
                       "&includeNotTracking=true"
                       "&includeNotPublic=true"
                       "&preferredTimezone=Europe/Prague";

    auto res = cli.Get(path, headers);
    if (!res) {
        logger.error("Failed to connect to Golem API.");
        return;
    }
    if (res->status == 401) {
        logger.error("Golem API: 401 Unauthorized -> check GOLEM_API_KEY.");
        return;
    }
    if (res->status != 200) {
        logger.error("Golem API: HTTP " + std::to_string(res->status));
        return;
    }

    auto json = nlohmann::json::parse(res->body, nullptr, false);
    if (json.is_discarded()) {
        logger.error("Golem API: Failed to parse JSON response.");
        return;
    }
    if (!json.contains("features") || !json["features"].is_array()) {
        logger.error("Golem API: unexpected response format (expected GeoJSON with features).");
        return;
    }

    realtimeDelays.clear();
    for (const auto& feature : json["features"]) {
        try {
            if (!feature.contains("properties"))
                continue;

            const auto& props = feature["properties"];

            // Extract trip ID
            // Sometimes we get a trip that does not exists in our DB so we just skit it - most likely some private bus
            std::string vehicleId;
            if (props.contains("trip") && props["trip"].contains("gtfs") && !props["trip"]["gtfs"]["trip_id"].is_null()) {
                vehicleId = props["trip"]["gtfs"]["trip_id"].get<std::string>();
            }
            if (vehicleId.empty())
                continue;

            // Get delay from last_position.delay.actual in seconds (positive = late, negative = early)
            int32_t delay = 0;
            if (props.contains("last_position") && props["last_position"].contains("delay") && props["last_position"]["delay"].contains("actual") && !props["last_position"]["delay"]["actual"].is_null()) {
                delay = props["last_position"]["delay"]["actual"].get<int32_t>();
            }
            realtimeDelays[gtfsHash(vehicleId)] = delay;
        } catch (...) { }
    }
}
bool Network::loadFromGTFS(const std::string& gtfsDir) {
    GTFSLoader loader(*this);
    return loader.loadFromGTFS(gtfsDir);
}

bool Network::loadFromAPI(APIEndpoint endpoint, const std::string& gtfsDir) {
    GTFSLoader loader(*this);
    return loader.loadFromAPI(endpoint, gtfsDir);
}