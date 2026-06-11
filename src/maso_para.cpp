#include "mhd_core.h"
#include "gtfs_loader.h"
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <future>
#include <mutex>

std::mutex g_logMutex;

struct SchoolLookup {
    std::string schoolName;
    std::string school_venue;
    Location startLocation;
    Location endLocation;
};

bool loadSchools(std::vector<SchoolLookup>& schools, const std::string& csvPath) {
    CSVParser parser;
    std::fstream csvFile(csvPath);
    if (!csvFile.is_open()) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "Error opening CSV file: " << csvPath << "\n";
        return false;
    }
    std::string line;
    std::getline(csvFile, line); // Skip header
    parser.parseHeader(line);
    while (std::getline(csvFile, line)) {
        std::string_view schoolName = parser.getField(line, "school_name");
        std::string_view schoolVenue = parser.getField(line, "school_venue");
        double latStart = std::stod(std::string(parser.getField(line, "lat_start")));
        double lonStart = std::stod(std::string(parser.getField(line, "lon_start")));
        double latEnd = std::stod(std::string(parser.getField(line, "lat_end")));
        double lonEnd = std::stod(std::string(parser.getField(line, "lon_end")));
        Location startLocation{latStart, lonStart};
        Location endLocation{latEnd, lonEnd};
        schools.push_back({std::string(schoolName), std::string(schoolVenue), startLocation, endLocation});
    }
    return true;
}

void getTimeForQuery(const std::string& dateStr, const std::string& timeStr, std::tm& tm) {
    strptime((dateStr + " " + timeStr).c_str(), "%d.%m.%Y %H:%M", &tm);
}

void findPathForSchool(MHDCore& core, const SchoolLookup& school, std::tm queryTm) {
    int threadSpecificQID = core.newQuery(queryTm, SearchPriority::QuickestTime, 1.3, 1000, 1.4);
    
    auto result = core.lookUpArrival(threadSpecificQID, school.startLocation, school.endLocation, 1, queryTm);
    
    if (result.status != "OK") {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "No path found for " << school.schoolName << " at venue " << school.school_venue << "\n";
        return;
    }
    
    std::string fileName = school.schoolName + "_" + school.school_venue + ".csv";
    std::ofstream outputFile(fileName);
    if (!outputFile.is_open()) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "Error opening output file: " << fileName << "\n";
        return;
    }
    
    core.exportPathsToCsv(threadSpecificQID, outputFile, CsvExportMode::AllStops);
    
    // Thread-safe console log
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << "Path for " << school.schoolName << " at venue " << school.school_venue << " exported to " << fileName << "\n";
}

int main(int argc, char** argv) {
    std::string gtfsDir = (argc > 1) ? argv[1] : "PID_GTFS";
    std::string lookupCsvPath;
    std::string dateArg;
    std::string timeArg;
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <GTFS_DIR> <path_to_lookup.csv> <dd.mm.yyyy> <hh:mm>\n";
        return 1;
    }
    lookupCsvPath = argv[2];
    dateArg = argv[3];
    timeArg = argv[4];

    bool onlineMode = true;
    MHDCore core("MHD_NET", onlineMode, Logger::Level::NONE); 
    if (!core.loadGTFS(gtfsDir, APIEndpoint::CUSTOM)) {
        return 2;
    }
    
    std::vector<SchoolLookup> schools;
    if (!loadSchools(schools, lookupCsvPath)) {
        return 3;
    }
    
    std::tm queryTm;
    getTimeForQuery(dateArg, timeArg, queryTm);

    std::cout << "Processing " << schools.size() << " schools in parallel using adaptive binary search...\n";

    std::vector<std::future<void>> futures;
    futures.reserve(schools.size());

    for (const auto& school : schools) {
        futures.push_back(std::async(std::launch::async, [&core, school, queryTm]() {
            findPathForSchool(core, school, queryTm);
        }));
    }

    for (auto& fut : futures) {
        fut.get();
    }

    std::cerr << "All paths processed.\n";
    return 0;
}