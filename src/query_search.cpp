#include "query_search.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_set>

uint32_t QuerySearch::applyRealtimeDelay(uint32_t arrivalTime, int32_t delaySeconds) {
    if (!delaySeconds)
        return arrivalTime;

    int64_t adjusted = static_cast<int64_t>(arrivalTime) + delaySeconds;
    if (adjusted < 0)
        adjusted += DateTimeUtils::DAYTIME;
    else if (adjusted >= DateTimeUtils::DAYTIME)
        adjusted %= DateTimeUtils::DAYTIME;
    return static_cast<uint32_t>(adjusted);
}

bool QuerySearch::PQComparator::operator()(const PQEntry& a, const PQEntry& b) const noexcept {
    if (priority == SearchPriority::QuickestTime) {
        if (a.day != b.day)
            return a.day > b.day;
        if (a.time != b.time)
            return a.time > b.time;
        if (a.changeNumber != b.changeNumber)
            return a.changeNumber > b.changeNumber;
        return a.walkSegments > b.walkSegments;
    }
    if (a.changeNumber != b.changeNumber)
        return a.changeNumber > b.changeNumber;
    if (a.walkSegments != b.walkSegments)
        return a.walkSegments > b.walkSegments;
    if (a.day != b.day)
        return a.day > b.day;
    return a.time > b.time;
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
        leg.arrivalTime = current.arrival_time;
        // Calculate arrival date based on the day offset from the lookup date
        leg.arrivalDate = DateTimeUtils::advanceDate(query.getLookupDate(), static_cast<int>((current.day + 7u - query.getDay()) % 7u));
        if (i + 1 >= routeIds.size()) {
            // Last leg, no departure time or line
            path.addLeg(leg);
            continue;
        }
        const auto& next = pathEntries[routeIds[i + 1]];
        leg.line = next.trip.line;
        leg.trip = next.trip;
        // kNoLine -> walking leg
        if (next.trip.line == kNoLine) {
            leg.isWalk = true;
            uint32_t nextArr = next.arrival_time;
            uint32_t curArr = current.arrival_time;
            // Handle midnight wrap for walking legs
            leg.walkSeconds = (nextArr >= curArr) ? (nextArr - curArr) : (DateTimeUtils::DAYTIMEu - curArr + nextArr);
            leg.departureTime = curArr;
        } else {
            uint32_t scheduledDep = current.arrival_time;
            if (next.trip.line < (line_id)tsIdx.size()) {
                // We detect lines but sometimes individual trips have special schedules so look up in the trip index for the exact departure time
                auto sit = tsIdx[next.trip.line].find(StopKey { next.trip, current.station });
                if (sit != tsIdx[next.trip.line].end())
                    scheduledDep = query.network.getLineSchedule(next.trip.line)[sit->second].arrival_time;
            }
            if (query.network.isOnlineMode()) {
                leg.delaySeconds = query.network.getRealtimeDelay(next.trip);
            }
            leg.departureTime = scheduledDep;
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
        mix(static_cast<uint64_t>(leg.arrivalTime));
        mix(static_cast<uint64_t>(leg.arrivalDate));
        mix(static_cast<uint64_t>(leg.departureTime));
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
        uint32_t newTime = cur.time + edge.seconds;
        uint8_t nd = cur.day;
        // Handle midnight wrap
        if (newTime >= DateTimeUtils::DAYTIME) {
            newTime -= DateTimeUtils::DAYTIME;
            nd = (nd + 1) % 7;
        }
        PathEntry p { (uint32_t)pathEntries.size(), edge.station, trip_id { }, newTime, nd, cur.entry_id };
        pathEntries.push_back(p);
        pq.push(PQEntry { trip_id { }, edge.station, p.entry_id, newTime, nd, (uint8_t)(cur.changeNumber + 1), (uint8_t)(cur.walkSegments + 1), false });
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
    // The trip might be active on the previous, current, or next as GTFS stores overlap of the day at midnight,
    // so check all three service days for possible departures within the allowed time window
    for (int dayShift = -1; dayShift <= 1; ++dayShift) {
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
        std::vector<PathEntry>& pathEntries, MinHeap& pq,
        uint8_t baseDay, const trip_id& trip,
        uint32_t boardServiceSec, int32_t selectedDep,
        size_t startIdx, const std::vector<ScheduleEntry>& sched,
        uint8_t currentStopSeq) {
    // Loop through the schedule entries and find our trip's next stops to expand to
    for (size_t di = startIdx; di < sched.size(); ++di) {
        const ScheduleEntry& dest = sched[di];
        if (dest.trip != trip)
            continue;
        // Only consider stops that are after the boarding stop in the trip sequence
        if (dest.stop_sequence <= currentStopSeq)
            continue;
        uint32_t destServiceSec = dest.day_offset * DateTimeUtils::DAYTIMEu + dest.arrival_time;
        if (destServiceSec < boardServiceSec)
            continue;
        // arr is calculated as the actual departure time from the boarding stop plus the scheduled time
        // difference between the boarding stop and the destination stop
        int32_t arr = selectedDep + static_cast<int32_t>(destServiceSec - boardServiceSec);

        int32_t delay = 0;
        if (query.network.isOnlineMode())
            delay = query.network.getRealtimeDelay(trip);
        uint32_t nextArr = QuerySearch::applyRealtimeDelay(arr, delay);
        arr += delay;
        int32_t arrWeek = arr % query.getWeekSeconds();
        if (arrWeek < 0)
            arrWeek += query.getWeekSeconds();
        // Calculate the arrival day and time considering the week and day wraps
        uint8_t arrDay = (uint8_t)((baseDay + (arrWeek / DateTimeUtils::DAYTIME)) % 7);

        uint8_t newChange = cur.changeNumber;
        if (cur.trip != trip && cur.trip.line != kNoLine && trip.line != kNoLine)
            ++newChange;
        PathEntry p { (uint32_t)pathEntries.size(), dest.station_id, trip, nextArr, arrDay, cur.entry_id };
        pathEntries.push_back(p);
        pq.push(PQEntry { trip, dest.station_id, p.entry_id, nextArr, arrDay, newChange, cur.walkSegments, false });
    }
}

void QuerySearch::expandTransit(const PQEntry& cur,
        std::vector<PathEntry>& pathEntries, MinHeap& pq,
        uint8_t baseDay) {
    // Check if we are in the max search horizon compared to the original query time
    uint32_t curAbsFromBase = static_cast<uint32_t>((cur.day - baseDay + 7) % 7) * DateTimeUtils::DAYTIMEu + cur.time;
    uint32_t baseQueryTime = query.getLookupTime();
    uint32_t elapsedFromQuery = (curAbsFromBase >= baseQueryTime)
            ? curAbsFromBase - baseQueryTime
            : curAbsFromBase + DateTimeUtils::DAYTIMEu - baseQueryTime;
    if (elapsedFromQuery > query.getLookupSearchHorizonSeconds())
        return;
    // apply transfer time and calculate the earliest and latest departure times for the next trip based on scan window
    const int32_t curDayStart = static_cast<int32_t>(((cur.day - baseDay + 7) % 7) * DateTimeUtils::DAYTIME);
    const int curDayOffset = static_cast<int>((cur.day - baseDay + 7) % 7);
    int32_t earliestDeparture = static_cast<int32_t>(curAbsFromBase);
    if (!cur.isWait && cur.entry_id != 0)
        earliestDeparture += static_cast<int32_t>(query.getDefaultTransferTime());
    const int32_t latestDeparture = earliestDeparture + query.getTransitScanWindowSeconds();

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
        const auto& indices = stIt->second;
        for (size_t boardIndex : indices) {
            const ScheduleEntry& board = sched[boardIndex];
            if (board.trip == cur.trip)
                continue;

            if (cur.trip.line != kNoLine && cur.trip.line == board.trip.line)
                continue;
            // look for best departure over midnight and week wrap
            uint32_t boardServiceSec = board.day_offset * DateTimeUtils::DAYTIMEu + board.arrival_time;
            auto selectedDep = findBestDeparture(board.trip, boardServiceSec,
                    curDayStart, curDayOffset, earliestDeparture, latestDeparture);

            if (!selectedDep)
                continue;
            // We have the boarding trip and departure time, now find the corresponding arrival at the destination stop to expand to
            size_t startIdx = (lid < (line_id)nextTripIdx.size() && boardIndex < nextTripIdx[lid].size())
                    ? nextTripIdx[lid][boardIndex]
                    : sched.size();
            if (startIdx >= sched.size())
                continue;
            // Now we can expand to the next stops of the boarding trip and add them to the queue
            processTripExpansion(cur, pathEntries, pq, baseDay, board.trip,
                    boardServiceSec, *selectedDep, startIdx, sched, board.stop_sequence);
        }
    }
}

std::optional<QuerySearch::PQEntry> QuerySearch::expandOneStep(
        MinHeap& pq,
        std::vector<PathEntry>& pathEntries,
        std::vector<uint32_t>& earliestArrival,
        uint8_t baseDay, uint32_t targetStation,
        std::vector<uint32_t>& lastWaitCache) {
    // Inner loop -> expand entries until we find one that is valid to expand
    while (!pq.empty()) {
        PQEntry cur = pq.top();
        pq.pop();
        uint32_t absTime = DateTimeUtils::toAbsSeconds(baseDay, cur.day, cur.time);
        // If we are optimizing for quickest time, we can prune entries that arrive later than the best known arrival for that station and day
        // however we cannot do this in the least transfers mode -> this explodes the search space and leads to much longer search times
        if (query.getSearchPriority() == SearchPriority::QuickestTime && !cur.isWait && cur.station != targetStation) {
            uint32_t idx = cur.station * 7u + (uint32_t)cur.day;
            uint32_t& inserted = earliestArrival[idx];
            if (inserted <= absTime)
                continue;
            inserted = absTime;
        }
        if (cur.changeNumber > query.getMaxTransfers())
            continue;

        expandWalking(cur, pathEntries, pq);
        expandTransit(cur, pathEntries, pq, baseDay);
        uint32_t origArrivalAbsTime = DateTimeUtils::toAbsSeconds(baseDay, pathEntries[cur.entry_id].day, pathEntries[cur.entry_id].arrival_time);
        if (absTime < origArrivalAbsTime + DateTimeUtils::DAYTIMEu) {
            // Schedule new wait step in the same station if it does not cause more than 24h difference from origo arrival
            PQEntry waitEntry = cur;
            waitEntry.time = cur.time + query.getDefaultWaitingTime(); // Default waiting time to expand the search window
            if (waitEntry.time >= DateTimeUtils::DAYTIMEu) {
                waitEntry.time -= DateTimeUtils::DAYTIMEu;
                waitEntry.day = (waitEntry.day + 1) % 7;
            }
            waitEntry.isWait = true;
            uint32_t waitIdx = waitEntry.station * 7u + (uint32_t)waitEntry.day;
            // cache it so we do not schedule multiple wait steps from diffrent arrivals in the same station
            uint32_t waitCache = waitEntry.time / query.getDefaultWaitingTime();
            if (waitIdx < lastWaitCache.size() && lastWaitCache[waitIdx] != waitCache) {
                lastWaitCache[waitIdx] = waitCache;
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

void QuerySearch::findPathsByStationIds(uint32_t startStationId,
        uint32_t endStationId, const int maxPaths) {
    int pathCount = 0;
    tm timeInfoCopy = query.timeInfo;
    tm now = { };
    time_t now_t = time(nullptr);
#ifdef _WIN32
    localtime_s(&now, &now_t);
#else
    localtime_r(&now_t, &now);
#endif
    mktime(&timeInfoCopy);
    // Check only if the lookup time is within 6 hours as we do not want to fetch realtime if not needed
    if (std::difftime(mktime(&timeInfoCopy), now_t) < 6 * 3600) {
        query.network.fetchRealtimeDelays();
    }
    // If either start or end station is invalid, return immediately
    if (!startStationId || !endStationId)
        return;
    // Return immediately if start and end stations are the same, with a single-leg path
    if (startStationId == endStationId) {
        auto path = Path();
        PathLeg leg { };
        leg.stationId = startStationId;
        leg.arrivalTime = query.getLookupTime();
        leg.arrivalDate = query.getLookupDate();
        path.addLeg(leg);
        query.paths.clear();
        query.paths.push_back(path);
        query.printRoute(query.paths.back(), pathCount, maxPaths);
        return;
    }

    const auto& tsIdx = query.network.getTripStopIndex();

    MinHeap pq(PQComparator { query.getSearchPriority() });
    std::vector<PathEntry> pathEntries;
    // Preallocate path entries to avoid frequent realloc
    pathEntries.reserve(1000000);

    uint32_t maxStationId = query.network.getMaxStationId();
    // For each station and day, store the earliest arrival time found so far to prune worse arrivals
    std::vector<uint32_t> earliestArrival((maxStationId + 1u) * 7u, std::numeric_limits<uint32_t>::max());

    query.paths.clear();
    // Cache to avoid scheduling multiple wait steps for the same station
    std::vector<uint32_t> lastWaitCache((maxStationId + 1u) * 7u, std::numeric_limits<uint32_t>::max());
    // Start with the initial station and time
    pathEntries.push_back({ 0, startStationId, trip_id { }, query.getLookupTime(), query.getDay(), std::numeric_limits<uint32_t>::max() });
    pq.push(PQEntry { trip_id { }, startStationId, 0, query.getLookupTime(), query.getDay(), 0, 0, false });

    const uint8_t baseDay = query.getDay();
    std::unordered_set<uint64_t> seenPathFingerprints;
    int expansions = 0;
    // Main search -> expand paths until we find enough paths or reach the expansion limit so we do not run indefinitely
    while (!pq.empty() && expansions < Query::maxExpansions && (int)query.paths.size() < maxPaths) {
        // Find first valid node to expand
        auto stepped = expandOneStep(pq, pathEntries, earliestArrival, baseDay, endStationId, lastWaitCache);
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
