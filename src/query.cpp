#include "query.h"
#include "network.h"
#include "query_search.h"
#include "stations.h"
#include <cstdint>
#include <memory>
#include <mutex>

void Query::writeTripSegmentCsvRow(std::ostream& output, const std::string& stationName,
        uint32_t arrivalDate, uint32_t arrivalTime, const Location& location, bool transferFlag) {
    output << csvEscape(stationName) << ','
           << formatDate(arrivalDate) << ','
           << formatTime(arrivalTime) << ','
           << location.latitude << ','
           << location.longitude << ',' << (transferFlag ? 1 : 0) << '\n';
}
void Query::buildWalkingIndex() {
    network.getLogger().info("Building walking index for " + std::string( sharedWalkingEdges ? "shared" : "query-specific" )+ " walking edges...");
    if (sharedWalkingEdges) {
        if (network.getWalkingIndex().empty()) {
            network.getLogger().info("Building shared walking index...");
            QuerySearch(*this).buildWalkingIndex();
            network.copyWalkingIndexFromQuery(this->walkingIndex);
            network.getLogger().info("Shared walking index successfully synchronized with " + 
                                     std::to_string(network.getWalkingIndex().size()) + " stations.");
            this->walkingIndex.clear();
        }
    }else{
        QuerySearch(*this).buildWalkingIndex();
    }
}

void Query::findPathsByStationIds(uint32_t startStationId,
        uint32_t endStationId, const int maxPaths) {
    QuerySearch(*this).findPathsByStationIds(startStationId, endStationId, maxPaths);
}

void Query::printRouteHeader(std::ostream& output, int& pathCount, size_t stopCount) const {
    output << "=== Path " << (++pathCount) << " === (" << stopCount << " stops)\n";
}

void Query::printRouteWalkingLeg(std::ostream& output, const PathLeg& leg, const PathLeg& nextLeg) const {
    output << " -> WALK " << (leg.walkSeconds / 60) << " min to "
           << network.getStationName(nextLeg.stationId)
           << " @ " << formatTime(nextLeg.arrivalTime);
}

void Query::printRouteTransitLeg(std::ostream& output, const PathLeg& leg, const PathLeg& nextLeg, bool isTransfer) const {
    std::string nextStationName = network.getStationName(nextLeg.stationId);
    output << " -> ";

    uint32_t actualDepTime = leg.departureTime;
    if (network.isOnlineMode() && leg.delaySeconds) {
        actualDepTime = QuerySearch::applyRealtimeDelay(leg.departureTime, leg.delaySeconds);
    }
    uint32_t waitTime = (actualDepTime >= leg.arrivalTime)
            ? (actualDepTime - leg.arrivalTime)
            : (DateTimeUtils::DAYTIME - leg.arrivalTime + actualDepTime);
    if (waitTime > 0 && waitTime < DateTimeUtils::DAYTIME) {
        output << "Wait " << (waitTime / 60) << " min";
        if (isTransfer)
            output << " (transfer)";
        output << " -> ";
    }

    uint32_t scheduledArrival = nextLeg.arrivalTime;
    if (leg.delaySeconds) {
        scheduledArrival = QuerySearch::applyRealtimeDelay(nextLeg.arrivalTime, -leg.delaySeconds);
    }
    output << "BOARD Line " << network.getLineName(leg.line) << " @ " << formatTime(leg.departureTime)
           << formatDelay(leg.delaySeconds) << " (arrive " << nextStationName << " @ "
           << formatTime(scheduledArrival) << formatDelayShort(leg.delaySeconds) << ")";
}

void Query::printRouteDestinationLeg(std::ostream& output) const {
    output << " -> DESTINATION";
}

// Get format in HH:MM
std::string Query::formatTime(uint32_t seconds) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u", seconds / 3600, (seconds % 3600) / 60);
    return buf;
}

// Get format like " (+5 min delay)"
std::string Query::formatDelay(int32_t delaySeconds) {
    if (delaySeconds == 0)
        return { };

    char sign = (delaySeconds > 0) ? '+' : '-';
    int32_t absDel = std::abs(delaySeconds);
    uint32_t minutes = (absDel + 30) / 60;

    if (minutes == 0)
        return { };

    char buf[32];
    std::snprintf(buf, sizeof(buf), " (%c%u min delay)", sign, minutes);
    return buf;
}

// Get format like "+5 min"
std::string Query::formatDelayShort(int32_t delaySeconds) {
    if (delaySeconds == 0)
        return { };
    int32_t absDel = std::abs(delaySeconds);
    uint32_t minutes = (absDel + 30) / 60;
    if (minutes == 0)
        return { };
    char buf[16];
    char sign = (delaySeconds > 0) ? '+' : '-';
    std::snprintf(buf, sizeof(buf), "%c%umin", sign, minutes);
    return buf;
}

// Get format in YYYY-MM-DD
std::string Query::formatDate(uint32_t yyyymmdd) {
    if (yyyymmdd == 0)
        return { };
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", yyyymmdd / 10000, (yyyymmdd / 100) % 100, yyyymmdd % 100);
    return buf;
}

// Escape a string for CSV output, adding quotes if necessary
std::string Query::csvEscape(std::string_view value) {
    bool needsQuotes = value.find_first_of(",\"\n\r") != std::string_view::npos;
    if (!needsQuotes)
        return std::string(value);
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"')
            escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool Query::isTransferRow(const std::vector<PathLeg>& legs, size_t i) {
    if (i == 0 || i >= legs.size())
        return false;
    const PathLeg& prev = legs[i - 1];
    const PathLeg& cur = legs[i];
    return !prev.isWalk && !cur.isWalk && prev.line != cur.line;
}

bool Query::printRoute(const Path& path, int& pathCount, int maxPaths) const {
    const auto& legs = path.getLegs();
    std::ostream* out = getOutput();
    if (out)
        printRouteHeader(*out, pathCount, legs.size());

    for (size_t i = 0; i < legs.size(); ++i) {
        const PathLeg& leg = legs[i];
        std::ostringstream oss;
        oss << (i + 1) << ". Arrive at " << network.getStationName(leg.stationId)
            << " @ " << formatTime(leg.arrivalTime);

        if (i + 1 < legs.size()) {
            const PathLeg& nextLeg = legs[i + 1];
            if (leg.isWalk) {
                printRouteWalkingLeg(oss, leg, nextLeg);
            } else {
                bool isTransfer = (i > 0 && !legs[i - 1].isWalk);
                printRouteTransitLeg(oss, leg, nextLeg, isTransfer);
            }
        } else {
            printRouteDestinationLeg(oss);
        }

        if (out)
            (*out) << oss.str() << '\n';
    }
    if (out)
        (*out) << '\n';
    return pathCount < maxPaths;
}

std::mutex mutex_local = std::mutex();

void Query::addWalkingToPathStart(Location startloc) {
    for (auto& path : paths) {
        if (path.getLegs().empty())
            continue;
        // Generate a staionID for this lookup if we don't have a valid start station
        uint32_t id = network.getStationByLocation(startloc);
        Location loc = network.getStationLocation(id);
        if (calcDistance(loc, startloc) < walkingSpeed * 60) { 
            continue;
        }
        mutex_local.lock();
        // else create new fake station and add walking leg to the start of the path
        int count = network.getStationCount();
        StationPtr station = std::make_unique<Station>(
                count, "Start Location", startloc, Platform(count, startloc).first);
        network.addStation(station);
        mutex_local.unlock();
        PathLeg leg { };
        leg.isWalk = true;
        leg.stationId = count;
        leg.arrivalDate = getLookupDate();
        leg.arrivalTime = getLookupTime();
        leg.departureTime = getLookupTime();
        leg.walkSeconds = static_cast<uint32_t>(calcDistance(startloc, network.getStationLocation(path.getLegs().front().stationId)) / walkingSpeed);
        Path newPath;
        newPath.addLeg(leg);
        for (const auto& l : path.getLegs()) {
            newPath.addLeg(l);
        }
        path = newPath;
    }
}

void Query::addWalkingToPathEnd(Location endloc) {
    for (auto& path : paths) {
        if (path.getLegs().empty()) continue;

        const PathLeg& lastTransitLeg = path.getLegs().back();
        uint32_t id = network.getStationByLocation(endloc);
        Location loc = network.getStationLocation(id);
        if (calcDistance(loc, endloc) < walkingSpeed * 60) { 
            continue;
        }
        mutex_local.lock();
        int count = network.getStationCount();
        StationPtr station = std::make_unique<Station>(
                count, "Start Location", endloc, Platform(count, endloc).first);
        network.addStation(station);
        mutex_local.unlock();
        PathLeg leg { };
        leg.isWalk = true;
        leg.stationId = count;
        leg.arrivalDate = lastTransitLeg.arrivalDate;
        leg.departureTime = lastTransitLeg.arrivalTime; 
        
        uint32_t walkDuration = static_cast<uint32_t>(calcDistance(endloc, network.getStationLocation(lastTransitLeg.stationId)) / walkingSpeed);
        leg.walkSeconds = walkDuration;
        leg.arrivalTime = lastTransitLeg.arrivalTime + walkDuration; // Arrives after walk duration

        path.addLeg(leg);
    }
}

// Export the segment of a trip leg from legs[i] to legs[j] (inclusive) as CSV rows
bool Query::exportTripSegmentCsv(std::ostream& output, const std::vector<PathLeg>& legs, size_t i, size_t j) const {
    // First check if this leg has valid line and trip info to look up the schedule
    const PathLeg& leg = legs[i];
    if (leg.line == kNoLine || leg.trip.id == 0)
        return false;
    line_id lid = leg.line;
    const auto& tripIndexVec = network.getTripStopIndex();
    if (lid >= tripIndexVec.size())
        return false;
    const auto& tripIndexMap = tripIndexVec[lid];
    StopKey skBoard { leg.trip, leg.stationId };
    // if last leg is transit, final station is the arrival station of that leg, otherwise it's the boarding station
    uint32_t finalStation = (j + 1 < legs.size()) ? legs[j + 1].stationId : legs[j].stationId;
    StopKey skFinal { leg.trip, finalStation };
    auto itB = tripIndexMap.find(skBoard);
    auto itA = tripIndexMap.find(skFinal);
    if (itB == tripIndexMap.end() || itA == tripIndexMap.end())
        return false;
    size_t idxB = itB->second; // index of boarding stop in line schedule
    size_t idxA = itA->second; // index of final stop in line schedule
    if (idxA < idxB)
        return false;

    // Lookup the schedule for this line and apply any realtime delay
    const auto& schedule = network.getLineSchedule(lid);
    int32_t delay = network.getRealtimeDelay(leg.trip);
    const auto& nextForLine = network.getNextTripScheduleIndex();
    if (lid < nextForLine.size()) {
        const auto& nextIdx = nextForLine[lid];
        size_t station = idxB;
        // Iterate through the schedule from boarding to final stop
        while (station < schedule.size()) {
            const ScheduleEntry& se = schedule[station];
            uint32_t arrTime = QuerySearch::applyRealtimeDelay(se.arrival_time, delay);
            uint32_t arrDate = leg.arrivalDate + se.day_offset;
            const Location& loc = network.getStationLocation(se.station_id);
            bool transferFlag = false;
            if (se.station_id == leg.stationId && isTransferRow(legs, i))
                transferFlag = true;
            if (se.station_id == finalStation && (j + 1 < legs.size()) && isTransferRow(legs, j + 1))
                transferFlag = true;
            writeTripSegmentCsvRow(output, network.getStationName(se.station_id), arrDate, arrTime, loc, transferFlag);
            if (station == idxA)
                break;
            if (station >= nextIdx.size())
                break;
            size_t n = nextIdx[station];
            if (n == station)
                break;
            station = n;
        }
        return true;
    }

    // If we don't have the next index for this line, fallback to a linear scan
    // YES GTFS can change the schedule to other stations between stops of the same line.
    for (size_t station = idxB; station <= idxA && station < schedule.size(); ++station) {
        const ScheduleEntry& se = schedule[station];
        uint32_t arrTime = QuerySearch::applyRealtimeDelay(se.arrival_time, delay);
        uint32_t arrDate = leg.arrivalDate + se.day_offset;
        const Location& loc = network.getStationLocation(se.station_id);
        bool transferFlag = false;
        if (se.station_id == leg.stationId && isTransferRow(legs, i))
            transferFlag = true;
        if (se.station_id == finalStation && (j + 1 < legs.size()) && isTransferRow(legs, j + 1))
            transferFlag = true;
        writeTripSegmentCsvRow(output, network.getStationName(se.station_id), arrDate, arrTime, loc, transferFlag);
    }
    return true;
}

void Query::exportFastestArrivalCsv(std::ostream& output, CsvExportMode mode, size_t pathIndex) const {
    const auto& paths = getPaths();
    if (pathIndex >= paths.size())
        return;

    const Path& path = paths[pathIndex];
    const auto& legs = path.getLegs();

    output << "station_name,arrival_date,arrival_time,latitude,longitude,line_change\n";

    for (size_t i = 0; i < legs.size(); ++i) {
        const PathLeg& leg = legs[i];
        // In TransfersOnly mode: print every station
        if (mode == CsvExportMode::TransfersOnly) {
            bool transferRow = isTransferRow(legs, i);
            const Location& location = network.getStationLocation(leg.stationId);
            writeTripSegmentCsvRow(output, network.getStationName(leg.stationId),
                    leg.arrivalDate, leg.arrivalTime, location, transferRow);
            continue;
        }

        // AllStops mode: expand transit legs to all scheduled stops between boarding and finaling
        if (mode == CsvExportMode::AllStops) {
            // Find contiguous segment of legs that belong to the same trip
            size_t j = i;
            while (j + 1 < legs.size() && !legs[j + 1].isWalk && legs[j + 1].trip.id == leg.trip.id)
                ++j;
            if (exportTripSegmentCsv(output, legs, i, j)) {
                i = j;
                continue;
            }
            if (leg.line != kNoLine && leg.trip.id != 0) {
                // if we intended to expand but failed, fallthrough to boarding-only row
                const Location& location = network.getStationLocation(leg.stationId);
                writeTripSegmentCsvRow(output, network.getStationName(leg.stationId),
                        leg.arrivalDate, leg.arrivalTime, location, false);
                continue;
            }
        }

        // Default fallback: print station row
        const Location& location = network.getStationLocation(leg.stationId);
        writeTripSegmentCsvRow(output, network.getStationName(leg.stationId),
                leg.arrivalDate, leg.arrivalTime, location, false);
    }
}

void Query::exportFastestArrivalCsv(const std::string& filePath, CsvExportMode mode, size_t pathIndex) const {
    std::ofstream file(filePath);
    if (!file.is_open())
        throw MHDException("Cannot open CSV output file: " + filePath);
    exportFastestArrivalCsv(file, mode, pathIndex);
}
