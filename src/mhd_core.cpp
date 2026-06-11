#include "mhd_core.h"

bool MHDCore::loadGTFS(const std::string& gtfsDir, APIEndpoint endpoint) {
    if (!network.loadFromAPI(endpoint, gtfsDir)) {
        network.getLogger().error("Failed to load GTFS data from API endpoint.");
        return false;
    }
    this->GTFSDir = gtfsDir;
    this->apiEndpoint = endpoint;
    return true;
}

int MHDCore::newQuery(tm currentTime, SearchPriority searchPriority,
        double walkingFactor, uint32_t maxWalkingDistance, double walkingSpeed, int maxTransfers,
        std::ostream* outStream) {
    querries.emplace_back(network, currentTime, searchPriority,
            walkingFactor, maxWalkingDistance, walkingSpeed, outStream, multiCoreMode);
    querries.back().setMaxTransfers(maxTransfers);
    return static_cast<int>(querries.size() - 1);
}

void MHDCore::exportPathsToCsv(const int queryID, std::ostream& output, CsvExportMode mode) {
    if (!isValidQueryID(queryID)) {
        output << "Invalid query ID\n";
        return;
    }
    Query& q = querries[queryID];
    q.exportFastestArrivalCsv(output, mode);
}

void MHDCore::printPaths(const int queryID, std::ostream& output) {
    if (!isValidQueryID(queryID)) {
        output << "Invalid query ID\n";
        return;
    }
    Query& q = querries[queryID];
    std::ostream* previus = q.getOutput();
    q.setOutput(&output);
    const Paths& p = q.getPaths();
    int pathCount = 0;
    for (const auto& up : p) {
        q.printRoute(up, pathCount, static_cast<int>(p.size()));
    }
    q.setOutput(previus);
}
bool MHDCore::refreshGTFS(const std::string& gtfsDir, APIEndpoint endpoint, bool onlineMode) {
    network.setOnlineMode(onlineMode);
    if (!network.loadFromAPI(endpoint, gtfsDir)) {
        network.getLogger().error("Failed to refresh GTFS data from API endpoint.");
        return false;
    }
    GTFSDir = gtfsDir;
    apiEndpoint = endpoint;
    return true;
}
