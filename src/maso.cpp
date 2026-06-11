#include "mhd_core.h"
#include "gtfs_loader.h"
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct SchoolLookup{
    std::string schoolName;
    std::string school_venue;
    Location startLocation;
    Location endLocation;
};

bool loadSchools(std::vector<SchoolLookup>& schools, const std::string& csvPath){
        CSVParser parser;
    std::fstream csvFile(csvPath);
    if (!csvFile.is_open()) {
        std::cerr << "Error opening CSV file: " << csvPath << "\n";
        return false;
    }
    std::string line;
    std::getline(csvFile, line); // Skip header
    parser.parseHeader(line);
    while(std::getline(csvFile, line)) {
        // for each line we need to get school_name, school_venue, lat_start, lon_start, lat_end, lon_end
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

void findPathForSchool(MHDCore& core, const SchoolLookup& school, std::tm queryTm){
    auto result = core.lookUpArrival(0, school.startLocation, school.endLocation, 1, queryTm);
    if (result.status != "OK") {
        std::cerr << "No path found for " << school.schoolName << " at venue " << school.school_venue << "\n";
        return;
    }
    // create csv file with name based on school name and venue, e.g. "SchoolName_Venue.csv"
    std::string fileName = school.schoolName + "_" + school.school_venue + ".csv";
    std::ofstream outputFile(fileName);
    if (!outputFile.is_open()) {
        std::cerr << "Error opening output file: " << fileName << "\n";
        return;
    }
    core.exportPathsToCsv(0, outputFile, CsvExportMode::AllStops);
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
    MHDCore core("MHD_NET", onlineMode, Logger::Level::INFO);
    if (!core.loadGTFS(gtfsDir, APIEndpoint::CUSTOM)) {
        return 2;
    }
    std::vector<SchoolLookup> schools;
    if (!loadSchools(schools, lookupCsvPath)) {
        return 3;
    }
    std::tm queryTm;
    getTimeForQuery(dateArg, timeArg, queryTm);
    // Maybe we can make this parallel but now it is just a concept
    auto qID = core.newQuery(queryTm);
    for (const auto& school : schools) {
        findPathForSchool(core, school, queryTm);
    }
    std::cerr << "All paths processed.\n";

    return 0;
}