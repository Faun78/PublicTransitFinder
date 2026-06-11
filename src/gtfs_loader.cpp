#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "gtfs_loader.h"
#include <ctime>
#include <fstream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_map>
#include <zip.h>

#ifdef _WIN32
constexpr double M_PI = 3.14159265358979323846;
#endif

static trip_id makeTripId(std::string_view raw) {
    return trip_id { gtfsHash(raw), kNoLine };
}

std::string_view CSVParser::csvFieldFixed(const std::string& line, size_t fieldIndex) const {
    const char* data = line.data();
    size_t len = line.size();
    size_t idx = 0;
    size_t i = 0;

    while (i < len) {
        size_t start = i;
        if (data[i] == '"') {
            ++i;
            start = i;
            while (i < len) {
                if (data[i] == '"') {
                    if (i + 1 < len && data[i + 1] == '"') {
                        i += 2;
                        continue;
                    }
                    break;
                }
                ++i;
            }
            size_t end = i;
            if (idx == fieldIndex)
                return std::string_view(data + start, end - start);
            if (i < len && data[i] == '"')
                ++i;
            if (i < len && data[i] == ',')
                ++i;
            ++idx;
        } else {
            while (i < len && data[i] != ',')
                ++i;
            size_t end = i;
            if (idx == fieldIndex)
                return std::string_view(data + start, end - start);
            if (i < len && data[i] == ',')
                ++i;
            ++idx;
        }
    }
    if (idx == fieldIndex)
        return std::string_view(data + len, 0);
    return std::string_view { };
}

void CSVParser::parseHeader(const std::string& headerLine) {
    header.clear();
    const char* data = headerLine.data();
    size_t len = headerLine.size();
    size_t idx = 0;
    size_t i = 0;

    while (i < len) {
        size_t start = i;
        if (data[i] == '"') {
            ++i;
            start = i;
            while (i < len && data[i] != '"')
                ++i;
            size_t end = i;
            std::string colName(data + start, end - start);
            header[colName] = idx;
            if (i < len && data[i] == '"')
                ++i;
            if (i < len && data[i] == ',')
                ++i;
        } else {
            while (i < len && data[i] != ',')
                ++i;
            size_t end = i;
            std::string colName(data + start, end - start);
            // Trim whitespace
            size_t first = colName.find_first_not_of(" \t\r\n");
            size_t last = colName.find_last_not_of(" \t\r\n");
            if (first != std::string::npos) {
                colName = colName.substr(first, last - first + 1);
            }
            header[colName] = idx;
            if (i < len && data[i] == ',')
                ++i;
        }
        ++idx;
    }
}
std::string_view CSVParser::getField(const std::string& line, const std::string& colName) const {
    auto it = header.find(colName);
    if (it == header.end())
        return std::string_view();
    return csvFieldFixed(line, it->second);
}
std::string_view CSVParser::getFieldByIndex(const std::string& line, size_t fieldIndex) const {
    return csvFieldFixed(line, fieldIndex);
}

bool CSVParser::hasColumn(const std::string& colName) const {
    return header.find(colName) != header.end();
}

static std::pair<std::string, std::string> parseUrl(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        throw MHDException("Invalid URL: " + url);
    size_t pathStart = url.find('/', scheme + 3);
    if (pathStart == std::string::npos)
        return { url.substr(scheme + 3), "/" };
    return { url.substr(scheme + 3, pathStart - scheme - 3),
        url.substr(pathStart) };
}

static bool extractZipArchive(Network& net, const std::string& zipPath, const std::string& absDir) {
    namespace fs = std::filesystem;

    int zipError = 0;
    zip_t* archive = zip_open(zipPath.c_str(), ZIP_RDONLY, &zipError);
    if (!archive) {
        net.getLogger().error("Extraction failed: cannot open ZIP archive.");
        return false;
    }

    auto fail = [&](const std::string& message) {
        net.getLogger().error(message);
        zip_close(archive);
        return false;
    };

    const fs::path baseDir = fs::absolute(absDir);
    const zip_int64_t entryCount = zip_get_num_entries(archive, 0);
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(entryCount); ++i) {
        const char* entryName = zip_get_name(archive, i, ZIP_FL_ENC_GUESS);
        if (!entryName)
            continue;

        fs::path relativePath(entryName);
        if (relativePath.is_absolute())
            return fail("Extraction failed: ZIP contains absolute path entry.");

        for (const auto& part : relativePath)
            if (part == "..")
                return fail("Extraction failed: ZIP contains unsafe parent traversal.");

        const size_t nameLen = std::char_traits<char>::length(entryName);
        if (nameLen == 0)
            continue;

        fs::path outPath = (baseDir / relativePath).lexically_normal();
        std::error_code ec;
        if (entryName[nameLen - 1] == '/') {
            fs::create_directories(outPath, ec);
            if (ec)
                return fail("Extraction failed: cannot create directory " + outPath.string());
            continue;
        }

        fs::create_directories(outPath.parent_path(), ec);
        if (ec)
            return fail("Extraction failed: cannot create parent directory for " + outPath.string());

        zip_file_t* file = zip_fopen_index(archive, i, 0);
        if (!file)
            return fail("Extraction failed: cannot open ZIP entry " + std::string(entryName));

        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            zip_fclose(file);
            return fail("Extraction failed: cannot write " + outPath.string());
        }

        char buffer[8192];
        for (;;) {
            zip_int64_t bytes = zip_fread(file, buffer, sizeof(buffer));
            if (bytes == 0)
                break;
            if (bytes < 0) {
                zip_fclose(file);
                return fail("Extraction failed: read error on ZIP entry " + std::string(entryName));
            }
            out.write(buffer, static_cast<std::streamsize>(bytes));
            if (!out) {
                zip_fclose(file);
                return fail("Extraction failed: write error for " + outPath.string());
            }
        }

        zip_fclose(file);
    }

    if (zip_close(archive) != 0) {
        return false;
    }
    return true;
}

// JSON loading helpers

void GTFSLoader::loadPlatformsFromJSON(const nlohmann::json& stopGroup, StationPtr& station) {
    if (!stopGroup.contains("stops"))
        return;
    for (const auto& stop : stopGroup["stops"]) {
        std::string platformIdstr = stop.value("id", std::string { });
        size_t pos = platformIdstr.find('/');
        if (pos != std::string::npos) {
            platformIdstr = platformIdstr.substr(pos + 1);
        }
        uint32_t platformId = 0;
        try {
            platformId = std::stoul(platformIdstr);
        } catch (...) {
            continue;
        }

        Location loc { stop.value("lat", 0.0), stop.value("lon", 0.0) };
        station->addPlatform(Platform(platformId, loc));
        if (stop.contains("gtfsIds") && stop["gtfsIds"].is_array()) {
            for (const auto& gid : stop["gtfsIds"]) {
                try {
                    network.registerGtfsStop(gid.get<std::string>(), station->getId());
                } catch (...) { }
            }
        }
    }
}

void GTFSLoader::loadStationsFromJSON(const nlohmann::json& data) {
    if (!data.contains("stopGroups"))
        return;

    for (const auto& stopGroup : data["stopGroups"]) {
        uint32_t stationId = 0;
        try {
            if (stopGroup.contains("node") && stopGroup["node"].is_number()) {
                stationId = stopGroup["node"].get<uint32_t>();
            } else {
                stationId = std::stoul(stopGroup.value("node", std::string("0")));
            }
            if (stationId == 0)
                continue;
        } catch (...) {
            continue;
        }

        std::string stationName = stopGroup.value("uniqueName", stopGroup.value("name", std::string { }));

        double lat = 0.0, lon = 0.0;
        if (stopGroup.contains("avgLat") && stopGroup.contains("avgLon")) {
            lat = stopGroup.value("avgLat", 0.0);
            lon = stopGroup.value("avgLon", 0.0);
        } else if (stopGroup.contains("lat") && stopGroup.contains("lon")) {
            lat = stopGroup.value("lat", 0.0);
            lon = stopGroup.value("lon", 0.0);
        }

        Location loc { lat, lon };
        StationPtr station = std::make_unique<Station>(
                stationId, stationName, loc, Platform(stationId, loc).first);
        loadPlatformsFromJSON(stopGroup, station);
        network.addStation(station);
    }
    network.buildPlatformParentVec();
    network.sortStationsByLocation();
}

void GTFSLoader::loadStationsFromCSV(const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open())
        throw MHDException("Cannot open stops.txt: " + filename);

    uint32_t nextStationId = 1;
    for (const auto& [stationId, location] : network.getStationsByLocation()) {
        (void)location;
        if (stationId >= nextStationId)
            nextStationId = stationId + 1;
    }

    std::string line;
    std::getline(f, line);

    CSVParser parser;

    parser.parseHeader(line);
    std::unordered_map<uint32_t, uint32_t> byAsw;
    std::unordered_map<std::string, uint32_t> byParentStation; // parent_station grouping
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto stop_id = parser.getField(line, "stop_id");
        auto stop_name = parser.getField(line, "stop_name");
        auto stop_lat_sv = parser.getField(line, "stop_lat");
        auto stop_lon_sv = parser.getField(line, "stop_lon");
        auto asw_node_sv = parser.getField(line, "asw_node_id");
        if (asw_node_sv.empty())
            asw_node_sv = parser.getField(line, "asw_node");

        auto asw_stop_sv = parser.getField(line, "asw_stop_id");
        if (asw_stop_sv.empty())
            asw_stop_sv = parser.getField(line, "asw_stop");

        if (stop_id.empty() || stop_name.empty() || stop_lat_sv.empty() || stop_lon_sv.empty())
            continue;

        // Check for parent_station (GTFS standard)
        auto parent_station = parser.getField(line, "parent_station");
        // PID specific aswNodes - used as internal id as they are kind of unique from
        // 1 otherwise we create our own counter `nextStationId`
        uint32_t aswNodeId = 0;
        uint32_t aswStopId = 0;
        try {
            if (!asw_node_sv.empty())
                aswNodeId = std::stoul(std::string(asw_node_sv));
            if (!asw_stop_sv.empty())
                aswStopId = std::stoul(std::string(asw_stop_sv));
        } catch (...) { }
        double stop_lat = 0.0, stop_lon = 0.0;
        try {
            stop_lat = std::stod(std::string(stop_lat_sv));
            stop_lon = std::stod(std::string(stop_lon_sv));
        } catch (...) {
            continue;
        }
        uint32_t stationId = 0;
        std::string stopKey = std::string(stop_id);

        // Use parent_station as grouping key if available, otherwise fallback to stop_id
        std::string canonicalParentKey = parent_station.empty() ? stopKey : std::string(parent_station);
        auto pit = byParentStation.find(canonicalParentKey);
        if (pit != byParentStation.end()) {
            stationId = pit->second;
        } else if (aswNodeId != 0) {
            auto ait = byAsw.find(aswNodeId);
            if (ait != byAsw.end()) {
                stationId = ait->second;
                byParentStation.emplace(canonicalParentKey, stationId);
            } else {
                stationId = nextStationId++;
                StationPtr s = std::make_unique<Station>(stationId, std::string(stop_name), Location { stop_lat, stop_lon }, aswStopId);
                network.addStation(s);
                byAsw.emplace(aswNodeId, stationId);
                byParentStation.emplace(canonicalParentKey, stationId);
            }
        } else {
            stationId = nextStationId++;
            StationPtr s = std::make_unique<Station>(stationId, std::string(stop_name), Location { stop_lat, stop_lon }, aswStopId);
            network.addStation(s);
            byParentStation.emplace(canonicalParentKey, stationId);
        }
        // We need to merge platforms based on parent_station
        network.registerPlatformToParent(stationId, stationId);
        network.registerGtfsStop(stopKey, stationId);

        if (parent_station.empty()) {
            byParentStation.emplace(stopKey, stationId);
        }
    }
    network.sortStationsByLocation();
}

/**
 * We support loading from JSON in the following format:
 * https://data.pid.cz/stops/json/stops.json
 *
 * But actually you just need few things from the JSON:
 * - Each station["stopGroups"] must have a unique id["node"], name["uniqueName"] and location (latitude["avgLat"] and longitude["avgLon"]).
 * - (Optional) Each station can have multiple platforms, which are represented as station["stopGroups"]["stops"]. Each platform must have a unique id["id"] and location
 */
void GTFSLoader::loadStationsFromFile(const std::string& filename) {
    if (filename.find(".txt") != std::string::npos) {
        loadStationsFromCSV(filename);
        return;
    }
    if (filename.find(".json") == std::string::npos)
        throw MHDException("Unsupported station file format: " + filename);

    if (filename.rfind("http", 0) == 0) {
        auto [host, path] = parseUrl(filename);
        httplib::SSLClient cli(host);
        cli.enable_server_certificate_verification(false);
        cli.set_connection_timeout(50, 0);
        auto res = cli.Get(path);
        if (!res)
            throw MHDException("Failed to connect to: " + host);
        if (res->status != 200)
            throw MHDException("HTTP " + std::to_string(res->status)
                    + " fetching stations from " + filename);
        loadStationsFromJSON(nlohmann::json::parse(res->body));
        return;
    }

    std::ifstream f(filename);
    if (!f.is_open())
        throw MHDException("Cannot open stations file: " + filename);
    loadStationsFromJSON(nlohmann::json::parse(f));
}

void GTFSLoader::loadRoutesFromFile(const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open())
        throw MHDException("Cannot open routes.txt: " + filename);

    std::string line;
    std::getline(f, line);

    CSVParser parser;
    parser.parseHeader(line);
    // We identify lines by route_short_name if available, otherwise fallback to route_id
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto route_id_sv = parser.getField(line, "route_id");
        auto route_short_sv = parser.getField(line, "route_short_name");
        if (route_id_sv.empty())
            continue;
        std::string name = route_short_sv.empty() ? std::string(route_id_sv) : std::string(route_short_sv);
        uint64_t lineHash = gtfsHash(route_short_sv.empty() ? route_id_sv : route_short_sv);
        line_id lineId = internLine(lineHash, name); // Internal line ID based on hash of route_short_name or route_id
        lineHashToId[gtfsHash(route_id_sv)] = lineId;
        if (!route_short_sv.empty())
            lineHashToId[gtfsHash(route_short_sv)] = lineId;
    }
}

void GTFSLoader::loadCalendarData(const std::string& gtfsDir) {
    std::string calendarPath = gtfsDir + "/calendar.txt";
    std::ifstream file(calendarPath);

    if (!file.is_open())
        return;

    std::string line;
    std::getline(file, line);

    CSVParser parser;
    parser.parseHeader(line);

    const char* days[] = { "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday" };
    while (std::getline(file, line)) {
        auto serviceIdStr = parser.getField(line, "service_id");
        if (serviceIdStr.empty())
            continue;

        service_id sid = internService(gtfsHash(serviceIdStr));
        Network::ServiceInfo& info = network.getServiceInfoVec()[sid];

        // Parse weekly schedule (1 = active, 0 = inactive)
        for (int dayIdx = 0; dayIdx < 7; ++dayIdx) {
            auto fieldValue = parser.getField(line, days[dayIdx]);
            if (!fieldValue.empty() && fieldValue[0] == '1')
                info.weekdays |= (uint8_t)(1 << dayIdx);
        }

        // Parse service validity date range
        info.start_date = DateTimeUtils::parseDateYYYYMMDD(
                parser.getField(line, "start_date"));
        info.end_date = DateTimeUtils::parseDateYYYYMMDD(
                parser.getField(line, "end_date"));
    }
}

void GTFSLoader::loadCalendarExceptions(const std::string& gtfsDir) {
    std::string exceptionsPath = gtfsDir + "/calendar_dates.txt";
    std::ifstream file(exceptionsPath);

    if (!file.is_open())
        return;

    std::string line;
    std::getline(file, line);

    CSVParser parser;
    parser.parseHeader(line);

    while (std::getline(file, line)) {
        auto serviceIdStr = parser.getField(line, "service_id");
        auto dateStr = parser.getField(line, "date");
        auto exceptionTypeStr = parser.getField(line, "exception_type");

        if (serviceIdStr.empty() || dateStr.empty() || exceptionTypeStr.empty())
            continue;
        uint32_t date = DateTimeUtils::parseDateYYYYMMDD(dateStr);
        int exceptionType = 0;
        try {
            exceptionType = std::stoi(std::string(exceptionTypeStr));
        } catch (...) {
            continue;
        }
        service_id sid = internService(gtfsHash(serviceIdStr));
        if (exceptionType == 2) {
            network.getCalendarExceptions()->removed[date].insert(sid);
        } else if (exceptionType == 1) {
            network.getCalendarExceptions()->added[date].insert(sid);
        }
    }
}

void GTFSLoader::loadTripRecords(const std::string& gtfsDir) {
    std::string filename = gtfsDir + "/trips.txt";
    std::ifstream file(filename);
    if (!file.is_open())
        throw MHDException("Cannot open trips file: " + filename);

    std::string line;
    std::getline(file, line);

    CSVParser parser;
    parser.parseHeader(line);
    while (std::getline(file, line)) {
        auto routeIdStr = parser.getField(line, "route_id");
        auto serviceIdStr = parser.getField(line, "service_id");
        auto tripIdStr = parser.getField(line, "trip_id");
        if (serviceIdStr.empty() || tripIdStr.empty())
            continue;
        // Create trip record with optional route
        trip_id tid = makeTripId(tripIdStr);

        if (!routeIdStr.empty()) {
            auto it = lineHashToId.find(gtfsHash(routeIdStr));
            if (it != lineHashToId.end())
                tid.line = it->second;
        }
        // To know when each trip is active, we link it to a service_id
        service_id sid = internService(gtfsHash(serviceIdStr));
        network.getTripServiceInfo()[tid.id] = sid;
        tripToLine[tid.id] = tid.line;
        auto itIdx = tripHashToIndex.find(tid.id);
        if (itIdx == tripHashToIndex.end()) {
            size_t newIdx = tripServiceVec.size();
            tripHashToIndex.emplace(tid.id, newIdx);
            tripServiceVec.push_back(sid);
        } else {
            tripServiceVec[itIdx->second] = sid;
        }

        // If service has no active days defined, assume it's active every day
        Network::ServiceInfo& info = network.getServiceInfoVec()[sid];
        if (info.weekdays == 0 && info.start_date == 0 && info.end_date == 0) {
            info.weekdays = 0xFF; // All days active
        }
    }
}

void GTFSLoader::loadTripsFromFile(const std::string& gtfsDir) {

    loadCalendarData(gtfsDir);
    loadCalendarExceptions(gtfsDir);
    loadTripRecords(gtfsDir);
}

void GTFSLoader::loadRouteStopsFromFile(const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open()) {
        network.getLogger().info("route_stops.txt not found, skipping.");
        return;
    }

    std::string line;
    std::getline(f, line);

    CSVParser parser;
    parser.parseHeader(line);

    while (std::getline(f, line)) {
        auto route_sv = parser.getField(line, "route_id");
        auto stopid_sv = parser.getField(line, "stop_id");
        if (route_sv.empty() || stopid_sv.empty())
            continue;
        auto it = lineHashToId.find(gtfsHash(route_sv));
        if (it == lineHashToId.end())
            continue;
        // We link each stop to the line it belongs to, so we can later quickly find which lines serve a station
        line_id lineId = it->second;
        uint32_t stationId = network.lookupStationByGtfsId(std::string(stopid_sv));
        auto sit = network.getStationsMap().find(stationId);
        if (sit == network.getStationsMap().end() || !sit->second)
            continue;

        auto& lines = sit->second->lines;
        if (std::find(lines.begin(), lines.end(), lineId) == lines.end())
            lines.push_back(lineId);
    }
}

void GTFSLoader::loadSchedulesFromFile(const std::string& filename) {
    std::ifstream f(filename);
    if (!f.is_open())
        throw MHDException("Cannot open schedules file: " + filename);

    std::string line;
    std::getline(f, line);

    CSVParser parser;
    parser.parseHeader(line);
    std::unordered_map<uint64_t, uint32_t> lastTripParentStation; // trip.id -> last parent station seen
    while (std::getline(f, line)) {
        auto trip_sv = parser.getField(line, "trip_id");
        auto arrival_sv = parser.getField(line, "arrival_time");
        auto departure_sv = parser.getField(line, "departure_time");
        auto stopid_sv = parser.getField(line, "stop_id");
        auto seq_sv = parser.getField(line, "stop_sequence");
        if (trip_sv.empty() || arrival_sv.empty() || stopid_sv.empty())
            continue;

        int arrival = DateTimeUtils::convertTimeToSeconds(arrival_sv);
        if (arrival < 0)
            continue;

        int departure = arrival;
        if (!departure_sv.empty()) {
            int d = DateTimeUtils::convertTimeToSeconds(departure_sv);
            if (d >= 0)
                departure = d;
        }

        int stopSeq = 0;
        for (char c : seq_sv) {
            if (c < '0' || c > '9') {
                stopSeq = 0;
                break;
            }
            stopSeq = stopSeq * 10 + (c - '0');
        }
        trip_id tid = makeTripId(trip_sv);
        auto tlIt = tripToLine.find(tid.id);
        if (tlIt != tripToLine.end())
            tid.line = tlIt->second;
        if (tid.line == kNoLine) {
            network.getLogger().error("Unresolved line for trip: " + std::string(trip_sv));
            continue;
        }
        uint32_t stationId = network.lookupStationByGtfsId(std::string(stopid_sv));
        if (stationId == 0)
            continue;
        // Link station to line if not already linked via route_stops.txt
        auto sit = network.getStationsMap().find(stationId);
        if (sit != network.getStationsMap().end()) {
            auto& lv = sit->second->lines;
            if (std::find(lv.begin(), lv.end(), tid.line) == lv.end())
                lv.push_back(tid.line);
        }
        tripToLine.emplace(tid.id, tid.line);
        if (tid.line >= network.getLineSchedules().size())
            network.getLineSchedules().resize(tid.line + 1);
        uint8_t dayOff = (uint8_t)((uint32_t)arrival / DateTimeUtils::DAYTIMEu);
        uint32_t wArr = (uint32_t)arrival % DateTimeUtils::DAYTIMEu, wDep = (uint32_t)departure % DateTimeUtils::DAYTIMEu;

        // Get parent station to check if this is a new station or same platform
        uint32_t parentId = network.getParentStation(stationId);
        auto lastPIt = lastTripParentStation.find(tid.id);
        uint32_t lastParent = (lastPIt != lastTripParentStation.end()) ? lastPIt->second : 0;

        // If same parent station (platform at same physical station), update last entry instead of adding new one
        if (lastParent == parentId && !network.getLineSchedules()[tid.line].empty() && network.getLineSchedules()[tid.line].back().trip == tid) {
            network.getLineSchedules()[tid.line].back().departure_time = wDep;
        } else {
            network.getLineSchedules()[tid.line].push_back(ScheduleEntry { wArr, wDep, tid, stationId, stopSeq, dayOff });
            lastTripParentStation[tid.id] = parentId;
        }
    }
    // Sort each line's schedule by arrival time
    for (auto& vec : network.getLineSchedules())
        if (!vec.empty())
            std::sort(vec.begin(), vec.end());
}

line_id GTFSLoader::internLine(uint64_t hash, const std::string& name) {
    auto [it, ins] = lineHashToId.emplace(hash, static_cast<line_id>(network.getLineSchedules().size()));
    if (ins) {
        network.getLineSchedules().emplace_back();
        network.getLineNameVec().push_back(name);
    } else if (!name.empty()) {
        network.getLineNameVec()[it->second] = name;
    }
    return it->second;
}

service_id GTFSLoader::internService(uint64_t hash) {
    auto [it, ins] = serviceHashToId.emplace(hash, static_cast<service_id>(network.getServiceInfoVec().size()));
    if (ins)
        network.getServiceInfoVec().emplace_back();
    return it->second;
}

// GTFS download

bool GTFSLoader::needsGtfsRefresh(const std::string& gtfsDir) const {

    std::string probe = gtfsDir + "/routes.txt";
    namespace fs = std::filesystem;
    std::error_code ec;
    auto ftime = fs::last_write_time(probe, ec);
    if (ec)
        return true; // file missing -> need refresh

    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    time_t fileTime = std::chrono::system_clock::to_time_t(sctp);
    return fileTime < std::time(nullptr) - 7 * 24 * 3600;
}

// Basicly detec API and download GTFS ZIP from there, then extract it to gtfsDir
bool GTFSLoader::downloadAndExtractGTFS(const std::string& gtfsDir) {
    if (!needsGtfsRefresh(gtfsDir)) {
        network.getLogger().info("GTFS data is up to date, skipping download.");
        return true;
    }
    network.getLogger().info("Downloading GTFS");

    namespace fs = std::filesystem;
    std::string absDir = fs::absolute(gtfsDir).string();
    std::string url = "";
    std::string zipUrl = "";
    std::string gtfsJsonUrl = "";
    if (network.getApiEndpoint() == APIEndpoint::GOLEM) {
        network.getLogger().info("Using Golem API for GTFS");
        const char* u = getenv("GOLEM_GTFS_URL");
        const char* z = getenv("GOLEM_GTFS_ZIP");
        const char* j = getenv("GOLEM_STOPS");
        if (u)
            url = u;
        if (z)
            zipUrl = z;
        if (j)
            gtfsJsonUrl = j;
    } else if (network.getApiEndpoint() == APIEndpoint::CUSTOM) {
        network.getLogger().info("Using custom API for GTFS");
        const char* u = getenv("CUSTOM_GTFS_URL");
        const char* z = getenv("CUSTOM_GTFS_ZIP");
        if (u)
            url = u;
        if (z)
            zipUrl = z;
    } else {
        network.getLogger().critical("Unsupported API endpoint for GTFS download.");
        return false;
    }
    if (url.empty() || zipUrl.empty()) {
        network.getLogger().critical("GTFS URLs not set in environment variables.");
        return false;
    }
    network.getLogger().info("Using GTFS URL: " + url + " and ZIP URL: " + zipUrl);

    std::string host = url;
    std::string scheme = "https";
    size_t scheme_pos = host.find("://");
    if (scheme_pos != std::string::npos) {
        scheme = host.substr(0, scheme_pos);
        host = host.substr(scheme_pos + 3);
    }
    if (!host.empty() && host.back() == '/') {
        host.pop_back();
    }

    std::string fullBaseUrl = scheme + "://" + host;
    auto cli = std::make_unique<httplib::Client>(fullBaseUrl);

    // Increase timeouts as GTFS zip is large
    cli->set_connection_timeout(60, 0);
    cli->set_read_timeout(300, 0); // Give it up to 5 minutes to download
    cli->set_follow_location(true);

#ifdef _WIN32
    cli->set_ca_cert_path(""); // skip CA verification for now
    cli->enable_server_certificate_verification(false);
#endif

    httplib::Headers headers = {
        { "Host", host },
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) mhd_finder/1.0" },
        { "Accept", "*/*" }
    };

    std::string path = zipUrl;
    if (path.empty() || path[0] != '/') {
        path = "/" + path;
    }
    if (fs::exists(absDir)) {
        std::error_code ec;
        fs::remove_all(absDir, ec);
        if (ec) {
            network.getLogger().error("Failed to clear GTFS directory: " + ec.message());
        }
    }
    fs::create_directories(absDir);
    std::string zipPath = absDir + "/temp_GTFS.zip";

    // Open file stream before making the request
    std::ofstream zf(zipPath, std::ios::binary);
    if (!zf) {
        network.getLogger().error("Cannot write " + zipPath);
        return false;
    }

    size_t downloadedBytes = 0;

    auto res = cli->Get(
            path,
            headers,
            [&](const httplib::Response& response) {
                if (response.status != 200) {
                    return false;
                }
                return true;
            },
            [&](const char* data, size_t data_length) {
                zf.write(data, static_cast<std::streamsize>(data_length));
                downloadedBytes += data_length;
                return true;
            });

    zf.close();

    if (!res || res->status != 200) {
        network.getLogger().error("Download failed"
                + (res ? ": HTTP " + std::to_string(res->status) : ""));
        network.getLogger().error(res.error() == httplib::Error::Success ? "Unknown error" : httplib::to_string(res.error()));
        network.getLogger().error("Attempted URL Path: " + path);

        std::error_code ec;
        fs::remove(zipPath, ec);
        return false;
    }
    network.getLogger().info("Downloaded " + std::to_string(downloadedBytes) + " bytes.");

    if (!extractZipArchive(network, zipPath, absDir))
        return false;

    {
        std::error_code ec;
        fs::remove(zipPath, ec);
    }
    if (network.getApiEndpoint() == APIEndpoint::GOLEM) {
        if (!gtfsJsonUrl.empty() && gtfsJsonUrl[0] != '/') {
            gtfsJsonUrl = "/" + gtfsJsonUrl;
        }

        auto res2 = cli->Get(gtfsJsonUrl, headers);
        if (res2 && res2->status == 200) {
            std::ofstream jsonf(absDir + "/stops.json");
            if (jsonf) {
                jsonf << res2->body;
                network.getLogger().info("Downloaded stops.json");
            } else {
                network.getLogger().error("Cannot write " + absDir + "/stops.json");
            }
        } else {
            network.getLogger().error("Failed to download stops.json"
                    + (res2 ? ": HTTP " + std::to_string(res2->status) : ""));
        }
    }

    network.getLogger().info("Extracted GTFS to " + absDir);
    return true;
}

// Auto load GTFS from GTFS dir
bool GTFSLoader::loadFromGTFS(const std::string& gtfsDir) {
    namespace fs = std::filesystem;
    std::string absDir = fs::absolute(gtfsDir).string();

    network.getLogger().info("Loading GTFS from " + absDir);

    loadRoutesFromFile(absDir + "/routes.txt");

    loadTripsFromFile(absDir);

    std::string stopsFile = absDir + "/stops.json";
    if (!std::ifstream(stopsFile).good()) {
        stopsFile = absDir + "/stops.txt";
        network.getLogger().info("stops.json not found, using stops.txt");
    }
    loadStationsFromFile(stopsFile);
    loadRouteStopsFromFile(absDir + "/route_stops.txt");
    loadSchedulesFromFile(absDir + "/stop_times.txt");

    // Build trip+station -> schedule index
    size_t lineCount = network.getLineSchedules().size();
    network.getTripStopIndex().resize(lineCount);
    network.getStationIndex().resize(lineCount);
    network.getNextTripScheduleIndex().resize(lineCount);
    for (line_id lid = 1; lid < (line_id)lineCount; ++lid) {
        const auto& sched = network.getLineSchedules()[lid];
        auto& ti = network.getTripStopIndex()[lid];
        auto& si = network.getStationIndex()[lid];
        auto& ni = network.getNextTripScheduleIndex()[lid];
        ti.reserve(sched.size());
        ni.assign(sched.size(), sched.size());

        std::unordered_map<uint64_t, std::vector<size_t>> tripIndices;
        tripIndices.reserve(sched.size());

        // Build both trip+station and station indices
        for (size_t i = 0; i < sched.size(); ++i) {
            ti.emplace(StopKey { sched[i].trip, sched[i].station_id }, i);
            si[sched[i].station_id].push_back(i); // Index for binary search by station
            tripIndices[sched[i].trip.id].push_back(i);
        }

        for (auto& [_, indices] : tripIndices) {
            std::sort(indices.begin(), indices.end(), [&sched](size_t a, size_t b) {
                return sched[a].stop_sequence < sched[b].stop_sequence;
            });
            for (size_t i = 0; i + 1 < indices.size(); ++i)
                ni[indices[i]] = indices[i + 1];
        }
    }

    network.getLogger().info("Loaded " + std::to_string(network.getStationCount()) + " stations, "
            + std::to_string(network.getTripCount()) + " trips, "
            + std::to_string(network.getLineCount()) + " lines.");
    return true;
}

// Main entry point for loading GTFS - tries to download from API if in online mode, otherwise loads from local GTFS dir
bool GTFSLoader::loadFromAPI(APIEndpoint endpoint, const std::string& gtfsDir) {
    if (endpoint == NONE)
        throw MHDException("Unsupported API endpoint");
    network.getApiEndpoint() = endpoint;
    namespace fs = std::filesystem;
    std::string absDir = fs::absolute(gtfsDir).string();
    if (network.getOnlineMode()) {
        if (!downloadAndExtractGTFS(absDir)) {
            if (!std::ifstream(absDir + "/stops.txt").good()
                    && !std::ifstream(absDir + "/stops.json").good())
                throw MHDException("No GTFS data available and download failed");
            network.getLogger().info("Using cached GTFS data.");
            network.setOnlineMode(false);
        }
    }

    return loadFromGTFS(absDir);
}
