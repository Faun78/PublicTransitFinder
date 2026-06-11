#ifndef GTFS_LOADER_H
#define GTFS_LOADER_H

#include "network.h"
#include <json.hpp>

struct CalendarDateExceptions {
    std::map<uint32_t, std::unordered_set<service_id>> removed;
    std::map<uint32_t, std::unordered_set<service_id>> added;
};

class CSVParser {
private:
    // Read csv field at fixed index, handling quoted fields and escaped quotes
    std::string_view csvFieldFixed(const std::string& line, size_t fieldIndex) const;

public:
    std::map<std::string, size_t> header; // column_name -> column_index

    // Parse header row and build column name to index mapping
    void parseHeader(const std::string& headerLine);
    // Get field by column name, returns empty string_view if column not found
    std::string_view getField(const std::string& line, const std::string& colName) const;
    // Get field by column index, returns empty string_view if index out of range
    std::string_view getFieldByIndex(const std::string& line, size_t fieldIndex) const;

    bool hasColumn(const std::string& colName) const;
};

class GTFSLoader {
private:
    Network& network;
    std::unordered_map<uint64_t, size_t> tripHashToIndex;
    std::vector<service_id> tripServiceVec;
    std::unordered_map<uint64_t, line_id> lineHashToId;
    std::unordered_map<uint64_t, service_id> serviceHashToId;
    std::unordered_map<uint64_t, line_id> tripToLine;

    void loadPlatformsFromJSON(const nlohmann::json& stopGroup, StationPtr& station);
    line_id internLine(uint64_t hash, const std::string& name = "");
    service_id internService(uint64_t hash);
    void loadStationsFromJSON(const nlohmann::json& data);
    void loadStationsFromCSV(const std::string& filename);
    void loadStationsFromFile(const std::string& filename);
    void loadRoutesFromFile(const std::string& filename);
    void loadTripsFromFile(const std::string& gtfsDir);
    void loadCalendarData(const std::string& gtfsDir);
    void loadCalendarExceptions(const std::string& gtfsDir);
    void loadTripRecords(const std::string& gtfsDir);
    void loadRouteStopsFromFile(const std::string& filename);
    void loadSchedulesFromFile(const std::string& filename);
    bool needsGtfsRefresh(const std::string& gtfsDir) const;
    bool downloadAndExtractGTFS(const std::string& gtfsDir);

public:
    GTFSLoader(Network& net)
        : network(net) { }

    bool loadFromAPI(APIEndpoint endpoint, const std::string& gtfsDir = "GTFS");
    bool loadFromGTFS(const std::string& gtfsDir);
};

#endif
