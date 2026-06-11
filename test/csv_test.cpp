#include "mhd_core.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    std::string gtfsDir = "PID_GTFS";

    if (argc > 1) {
        gtfsDir = argv[1];
    }
    MHDCore core("MHD_NET_TEST", false, Logger::Level::NONE);
    try {
        core.loadFromGTFS(gtfsDir);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load GTFS: " << e.what() << std::endl;
        return 2;
    }
    tm currentTime = { };
    std::time_t now = std::time(nullptr);
    std::tm* now_tm = std::localtime(&now);
    if (now_tm)
        currentTime = *now_tm;

    int qid = core.newQuery(currentTime, SearchPriority::QuickestTime, 1.3, 1000, 1.4);
    auto res = core.lookUp(qid, "Pelc Tyrolka", "Vrchlabí");
    if (res.paths.empty()) {
        std::cerr << "No paths found in csv_test" << std::endl;
        return 3;
    }

    const std::string outFile = "csv_test_out.csv";
    const std::string outFileAll = "csv_test_all_out.csv";
    try {
        std::ofstream of(outFile);
        if (!of)
            return 5;
        core.exportPathsToCsv(qid, of, CsvExportMode::TransfersOnly);
        of.close();
        std::ofstream of2(outFileAll);
        if (!of2)
            return 5;
        core.exportPathsToCsv(qid, of2, CsvExportMode::AllStops);
        of2.close();
    } catch (...) {
        return 4;
    }

    auto check = [&](const std::string& f) {
        std::ifstream in(f);
        if (!in.good()) {
            return 5;
        }
        std::string header;
        std::getline(in, header);
        if (header.find("station_name") == std::string::npos) {
            return 6;
        }
        std::string firstLine;
        if (!std::getline(in, firstLine)) {
            return 7;
        }
        return 0;
    };

    int r1 = check(outFile);
    if (r1 != 0)
        return r1;
    int r2 = check(outFileAll);
    if (r2 != 0)
        return r2;
    std::cout << "csv_test: OK, wrote " << outFile << std::endl;
    return 0;
}
