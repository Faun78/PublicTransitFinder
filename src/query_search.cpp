#include "query_search.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_set>
#include <cmath>

uint32_t QuerySearch::applyRealtimeDelay(uint32_t arrivalTime, int32_t delaySeconds) {
    int64_t adjusted = static_cast<int64_t>(arrivalTime) + delaySeconds;
    if (adjusted < 0)
        return 0;
    return static_cast<uint32_t>(adjusted);
}

bool QuerySearch::PQComparator::operator()(const PQEntry& a, const PQEntry& b) const noexcept {
    if (priority == SearchPriority::QuickestTime) {
        if (a.elapsed_seconds != b.elapsed_seconds)
            return a.elapsed_seconds > b.elapsed_seconds;
        if (a.changeNumber != b.changeNumber)
            return a.changeNumber > b.changeNumber;
        return a.walkSegments > b.walkSegments;
    }
    // Least Transfers Mode
    if (a.changeNumber != b.changeNumber)
        return a.changeNumber > b.changeNumber;
    if (a.walkSegments != b.walkSegments)
        return a.walkSegments > b.walkSegments;
    return a.elapsed_seconds > b.elapsed_seconds;
}

Path QuerySearch::buildPath(const PathEntry& endEntry,
        const std::vector<QuerySearch::PathEntry>& pathEntries,
        const std::vector<TripStopIndex>& tsIdx) const {
    std::vector<uint32_t> routeIds;
    {
        // Backtrack from the end entry to reconstruct the path
        uint32_t id = endEntry.entry_id;
        while (id != std::numeric_limits<uint32_t>::max()) {
            routeIds.push_back(id);
            id = pathEntries[id].prev;
        }
    }
    std::reverse(routeIds.begin(), routeIds.end());
    auto path = Path();

    for (size_t i = 0; i < routeIds.size(); ++i) {
        const auto& current = pathEntries[routeIds[i]];
        PathLeg leg { };
        leg.stationId = current.station;
        leg.elapsedArrival = current.elapsed_seconds;

        if (i + 1 >= routeIds.size()) {
            // Koncová stanice trasy nedisponuje odjezdem
            leg.elapsedDeparture = current.elapsed_seconds;
            path.addLeg(leg);
            continue;
        }

        const auto& next = pathEntries[routeIds[i + 1]];
        leg.line = next.trip.line;
        leg.trip = next.trip;
        // kNoLine -> walking leg
        if (next.trip.line == kNoLine) {
            leg.isWalk = true;
            leg.walkSeconds = next.elapsed_seconds - current.elapsed_seconds;
            leg.elapsedDeparture = current.elapsed_seconds;
            leg.elapsedArrival = next.elapsed_seconds; 
        } else {
            uint32_t elapsedDeparture = current.elapsed_seconds;
            if (next.trip.line < (line_id)tsIdx.size()) {
                // We detect lines but sometimes individual trips have special schedules so look up in the trip index for the exact departure time
                auto sit = tsIdx[next.trip.line].find(StopKey { next.trip, current.station });
                if (sit != tsIdx[next.trip.line].end()) {
                    const auto& sched = query.network.getLineSchedule(next.trip.line);
                    const auto& schedEntry = sched[sit->second];
                    
                    uint32_t boardServiceSec = schedEntry.day_offset * DateTimeUtils::DAYTIME + schedEntry.arrival_time;
                    
                    uint32_t baseTimeSec = query.getLookupTime();
                    int32_t earliestDep = static_cast<int32_t>(baseTimeSec + current.elapsed_seconds);
                    if (i > 0 && !(pathEntries[routeIds[i]].trip.line == kNoLine)) {
                        earliestDep += static_cast<int32_t>(query.getDefaultTransferTime());
                    }

                    int32_t selectedDep = std::numeric_limits<int32_t>::max();
                    int32_t realtimeDelay = query.network.isOnlineMode() ? query.network.getRealtimeDelay(next.trip) : 0;

                    for (int dayShift = -1; dayShift <= 2; ++dayShift) {
                        int32_t depCandidate = dayShift * DateTimeUtils::DAYTIME + static_cast<int32_t>(boardServiceSec);
                        if (depCandidate + realtimeDelay >= earliestDep) {
                            if (depCandidate < selectedDep) {
                                int32_t totalSecFromBase = depCandidate;
                                int32_t daysFromLookup = totalSecFromBase / static_cast<int32_t>(DateTimeUtils::DAYTIME);
                                if (totalSecFromBase < 0 && totalSecFromBase % DateTimeUtils::DAYTIME != 0) {
                                    daysFromLookup--;
                                }
                                uint32_t depDate = DateTimeUtils::advanceDate(query.getLookupDate(), daysFromLookup);
                                
                                if (query.network.isTripActiveOnDate(next.trip, depDate)) {
                                    selectedDep = depCandidate;
                                }
                            }
                        }
                    }

                    if (selectedDep != std::numeric_limits<int32_t>::max()) {
                        elapsedDeparture = static_cast<uint32_t>(selectedDep - static_cast<int32_t>(baseTimeSec));
                    }
                }
            }

            if (query.network.isOnlineMode()) {
                leg.delaySeconds = query.network.getRealtimeDelay(next.trip);
            }
            leg.elapsedDeparture = elapsedDeparture;
        }
        path.addLeg(leg);
    }
    return path;
}

uint64_t QuerySearch::pathFingerprint(const Path& path) {
    // Random prime numbers for FNV-1a hash
    uint64_t hash = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;

    for (const auto& leg : path.getLegs()) {
        auto mix = [&](uint64_t value) {
            hash ^= value;
            hash *= prime;
        };

        mix(static_cast<uint64_t>(leg.stationId));
        mix(static_cast<uint64_t>(leg.elapsedArrival));
        mix(static_cast<uint64_t>(leg.elapsedDeparture));
        mix(static_cast<uint64_t>(leg.line));
        mix(static_cast<uint64_t>(leg.delaySeconds));
        mix(static_cast<uint64_t>(leg.isWalk));
        mix(static_cast<uint64_t>(leg.walkSeconds));
    }

    return hash;
}

void QuerySearch::expandWalking(const PQEntry& cur,
        std::vector<PathEntry>& pathEntries, MinHeap& pq) {
    const auto& index = query.getWalkingIndex();
    auto it = index.find(cur.station);
    if (it == index.end())
        return;
    for (const auto& edge : it->second) {
        if (edge.station == cur.station || edge.seconds == 0)
            continue;

        uint32_t nextElapsed = cur.elapsed_seconds + edge.seconds;
        PathEntry p { (uint32_t)pathEntries.size(), edge.station, trip_id { }, nextElapsed, cur.entry_id };
        pathEntries.push_back(p);
        pq.push(PQEntry { trip_id { }, edge.station, p.entry_id, nextElapsed,
                (uint8_t)(cur.changeNumber + 1), (uint8_t)(cur.walkSegments + 1), false });
    }
}

// Finds the best departure time for a given trip considering service schedules and real-time delays
std::optional<int32_t> QuerySearch::findBestDeparture(const trip_id& trip, uint32_t boardServiceSec,
        int32_t curDayStart, int curDayOffset,
        int32_t earliestDeparture, int32_t latestDeparture) {
    int32_t selectedDep = std::numeric_limits<int32_t>::max();
    int32_t realtimeDelay = 0;
    if (query.network.isOnlineMode())
        realtimeDelay = query.network.getRealtimeDelay(trip);

    int checkRange = 1 + static_cast<int>(boardServiceSec / DateTimeUtils::DAYTIME);
    for (int dayShift = -checkRange; dayShift <= 1; ++dayShift) {
        int32_t serviceStart = curDayStart + dayShift * DateTimeUtils::DAYTIME;
        int32_t depCandidate = serviceStart + static_cast<int32_t>(boardServiceSec);
        if (depCandidate < earliestDeparture || depCandidate > latestDeparture)
            continue;

        int32_t actualDepCandidate = depCandidate + realtimeDelay;
        if (actualDepCandidate < earliestDeparture || actualDepCandidate > latestDeparture)
            continue;

        int depDateOffset = curDayOffset + dayShift;
        uint32_t depDate = DateTimeUtils::advanceDate(query.getLookupDate(), depDateOffset);
        if (!query.network.isTripActiveOnDate(trip, depDate))
            continue;

        if (depCandidate < selectedDep)
            selectedDep = depCandidate;
    }
    if (selectedDep == std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return selectedDep;
}

void QuerySearch::processTripExpansion(const PQEntry& cur,
        std::vector<QuerySearch::PathEntry>& pathEntries, MinHeap& pq,
        uint32_t baseQueryTime, const ScheduleEntry& board,
        uint32_t boardServiceSec, int32_t selectedDep,
        size_t startIdx, const std::vector<ScheduleEntry>& sched) {

    // Expand to onward stops
    for (size_t di = startIdx; di < sched.size(); ++di) {
        const ScheduleEntry& dest = sched[di];
        if (dest.trip != board.trip)
            continue;
        if (dest.stop_sequence <= board.stop_sequence)
            continue;

        uint32_t destServiceSec = dest.day_offset * DateTimeUtils::DAYTIME + dest.arrival_time;
        if (destServiceSec < boardServiceSec)
            continue;

        int32_t arrAbs = selectedDep + static_cast<int32_t>(destServiceSec - boardServiceSec);
        int32_t delay = query.network.isOnlineMode() ? query.network.getRealtimeDelay(board.trip) : 0;
        arrAbs += delay;

        if (arrAbs < static_cast<int32_t>(baseQueryTime))
            continue;
        uint32_t nextElapsed = static_cast<uint32_t>(arrAbs - baseQueryTime);

        uint8_t newChange = cur.changeNumber;
        if (cur.trip != board.trip && cur.trip.line != kNoLine)
            ++newChange;

        PathEntry p { (uint32_t)pathEntries.size(), dest.station_id, board.trip, nextElapsed, cur.entry_id };
        pathEntries.push_back(p);
        pq.push(PQEntry { board.trip, dest.station_id, p.entry_id, nextElapsed, newChange, cur.walkSegments, false });
    }
}

void QuerySearch::expandTransit(const PQEntry& cur,
        std::vector<QuerySearch::PathEntry>& pathEntries, MinHeap& pq) {
    if (cur.elapsed_seconds > query.getLookupSearchHorizonSeconds())
        return;

    // Shift window relative to base lookup midnight context
    uint32_t baseQueryTime = query.getLookupTime();
    int32_t earliestDeparture = static_cast<int32_t>(baseQueryTime + cur.elapsed_seconds);
    if (!cur.isWait && cur.entry_id != 0)
        earliestDeparture += static_cast<int32_t>(query.getDefaultTransferTime());
    int32_t latestDeparture = earliestDeparture + query.getTransitScanWindowSeconds();

    const auto& stationIdx = query.network.getStationIndex();
    const auto& nextTripIdx = query.network.getNextTripScheduleIndex();

    // Loop through lines serving the current station and try to find trips to expand to
    for (const line_id lid : query.network.getLinesServingStation(cur.station)) {
        // Some trips might have invalid line_id due to missing or inconsistent data, skip those
        if (lid == kNoLine || lid >= query.network.getLineScheduleCount()) {
            query.network.getLogger().error("Line not in index: " + std::to_string(lid));
            continue;
        }
        const auto& sched = query.network.getLineSchedule(lid);
        // Look up the station id's for the line in the station index to find candidate trips to board
        // we cannot seach just by line as not every trip stops at every station on the line
        if (lid >= (line_id)stationIdx.size()) {
            continue;
        }
        auto stIt = stationIdx[lid].find(cur.station);
        if (stIt == stationIdx[lid].end())
            continue;
        // When we find the station in the index, we get a list of schedule indices
        // corresponding to trips that stop at the station, loop through those to find valid trips to board
        for (size_t boardIndex : stIt->second) {
            const ScheduleEntry& board = sched[boardIndex];
            if (board.trip == cur.trip)
                continue;
            if (cur.trip.line != kNoLine && cur.trip.line == board.trip.line)
                continue;

            uint32_t boardServiceSec = board.day_offset * DateTimeUtils::DAYTIME + board.arrival_time;

            // Pass constant context targets relative to day 0 references
            auto selectedDep = findBestDeparture(board.trip, boardServiceSec, 0, 0, earliestDeparture, latestDeparture);
            if (!selectedDep)
                continue;
            // We have the boarding trip and departure time, now find the corresponding arrival at the destination stop to expand to
            size_t startIdx = (lid < (line_id)nextTripIdx.size() && boardIndex < nextTripIdx[lid].size())
                    ? nextTripIdx[lid][boardIndex]
                    : sched.size();
            if (startIdx >= sched.size())
                continue;
            // Now we can expand to the next stops of the boarding trip and add them to the queue
            processTripExpansion(cur, pathEntries, pq, baseQueryTime, board,
                    boardServiceSec, *selectedDep, startIdx, sched);
        }
    }
}

std::optional<QuerySearch::PQEntry> QuerySearch::expandOneStep(
        MinHeap& pq,
        std::vector<PathEntry>& pathEntries,
        std::vector<uint32_t>& earliestArrival,
        uint32_t targetStation, std::vector<uint32_t>& lastWaitCache) {
    // Inner loop -> expand entries until we find one that is valid to expand
    while (!pq.empty()) {
        PQEntry cur = pq.top();
        pq.pop();
        if (query.getSearchPriority() == SearchPriority::QuickestTime && !cur.isWait && cur.station != targetStation) {
            uint32_t& currentBest = earliestArrival[cur.station];
            if (currentBest <= cur.elapsed_seconds)
                continue;
            currentBest = cur.elapsed_seconds;
        }
        if (cur.changeNumber > query.getMaxTransfers())
            continue;

        expandWalking(cur, pathEntries, pq);
        expandTransit(cur, pathEntries, pq);

        // Schedule next incremental waiting step safely inside bounds
        uint32_t origElapsed = pathEntries[cur.entry_id].elapsed_seconds;
        if (cur.elapsed_seconds < origElapsed + DateTimeUtils::DAYTIME) {
            PQEntry waitEntry = cur;
            waitEntry.elapsed_seconds += query.getDefaultWaitingTime();
            waitEntry.isWait = true;
            // cache it so we do not schedule multiple wait steps from diffrent arrivals in the same station
            uint32_t waitCache = waitEntry.elapsed_seconds / query.getDefaultWaitingTime();
            if (lastWaitCache[cur.station] != waitCache) {
                lastWaitCache[cur.station] = waitCache;
                pq.push(waitEntry);
            }
        }
        return cur;
    }
    return std::nullopt;
}

void QuerySearch::buildWalkingIndex() {
    query.walkingIndex.clear();
    const double maxMeters = static_cast<double>(query.maxWalkingDistance);
    const double factor = query.walkingFactor;
    const double speedMps = query.walkingSpeed;
    const auto& byLoc = query.network.getStationsByLocation();
    // Building walking edges for all pairs of stations within maxWalkingDistance, excluding those on the same parent station
    for (size_t i = 0; i < byLoc.size(); ++i) {
        uint32_t idA = byLoc[i].first;
        const Location& locA = byLoc[i].second;

        uint32_t parentA = query.network.getParentStation(idA);
        for (size_t j = i + 1; j < byLoc.size(); ++j) {
            uint32_t idB = byLoc[j].first;
            uint32_t parentB = query.network.getParentStation(idB);
            if (parentA == parentB)
                continue;
            // To speed things up firstly we do a quick check on latitude and longitude differences in meters
            const Location& locB = byLoc[j].second;
            double latDiffMeters = std::abs(locB.latitude - locA.latitude) * query.degToMeters;
            if (latDiffMeters > maxMeters)
                continue;
            double lonDiffMeters = std::abs(locB.longitude - locA.longitude) * query.degToMeters * std::cos(locA.latitude * query.degToRadians);
            if (lonDiffMeters > maxMeters)
                continue;
            // Haverstine distance is more expensive so we calc it after the quick bounding box check
            double dist = calcDistance(locA, locB) * factor;
            if (dist > maxMeters)
                continue;
            uint32_t secs = static_cast<uint32_t>(dist / speedMps);
            if (secs == 0)
                // For some reasons there are stations that do not have
                // the parent station set so skip walking edges between them
                continue;

            query.walkingIndex[idA].push_back({ idB, secs });
            query.walkingIndex[idB].push_back({ idA, secs });
        }
    }
}

void QuerySearch::findPathsByStationIds(uint32_t startStationId, uint32_t endStationId, const int maxPaths) {
    int pathCount = 0;
    if (!startStationId || !endStationId)
        return;
    // Return immediately if start and end stations are the same, with a single-leg path
    if (startStationId == endStationId) {
        auto path = Path();
        PathLeg leg { startStationId, 0, 0 };
        path.addLeg(leg);
        query.paths = { path };
        query.printRoute(query.paths.back(), pathCount, maxPaths);
        return;
    }

    const auto& tsIdx = query.network.getTripStopIndex();
    MinHeap pq(PQComparator { query.getSearchPriority() });
    std::vector<PathEntry> pathEntries;
    pathEntries.reserve(500000);

    uint32_t maxStationId = query.network.getMaxStationId();
    std::vector<uint32_t> earliestArrival(maxStationId + 1, std::numeric_limits<uint32_t>::max());
    // Cache to avoid scheduling multiple wait steps for the same station
    std::vector<uint32_t> lastWaitCache(maxStationId + 1, std::numeric_limits<uint32_t>::max());

    query.paths.clear();

    // Start node has 0 elapsed seconds
    pathEntries.push_back({ 0, startStationId, trip_id { }, 0, std::numeric_limits<uint32_t>::max() });
    pq.push(PQEntry { trip_id { }, startStationId, 0, 0, 0, 0, false });

    std::unordered_set<uint64_t> seenPathFingerprints;
    int expansions = 0;
    // Main search -> expand paths until we find enough paths or reach the expansion limit so we do not run indefinitely
    while (!pq.empty() && expansions < Query::maxExpansions && (int)query.paths.size() < maxPaths) {
        // Find first valid node to expand
        auto stepped = expandOneStep(pq, pathEntries, earliestArrival, endStationId, lastWaitCache);
        if (!stepped)
            continue;
        ++expansions;
        // If we reached the target station, build the path and add it to results if it's not a duplicate(detected by fingerprint)
        if (stepped->station == endStationId) {
            auto path = buildPath(pathEntries[stepped->entry_id], pathEntries, tsIdx);
            uint64_t printedFingerprint = pathFingerprint(path);
            if (!seenPathFingerprints.insert(printedFingerprint).second)
                continue;
            query.paths.push_back(path);
            // Print till we reach the max path limit
            if (!query.printRoute(query.paths.back(), pathCount, maxPaths))
                break;
        }
    }
}
