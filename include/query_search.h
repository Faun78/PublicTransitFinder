#ifndef QUERY_SEARCH_H
#define QUERY_SEARCH_H

#include "query.h"
#include <optional>
#include <queue>

class QuerySearch {
private:
    Query& query;

    struct PQEntry {
        trip_id trip;
        uint32_t station;
        uint32_t entry_id;
        uint32_t elapsed_seconds;
        uint8_t changeNumber;
        uint8_t walkSegments;
        bool isWait = false;
    };

    struct PQComparator {
        SearchPriority priority = SearchPriority::QuickestTime;
        bool operator()(const PQEntry& a, const PQEntry& b) const noexcept;
    };

    struct PathEntry {
        uint32_t entry_id;
        uint32_t station;
        trip_id trip;
        uint32_t elapsed_seconds;
        uint32_t prev;
    };

    using MinHeap = std::priority_queue<PQEntry, std::vector<PQEntry>, PQComparator>;

    Path buildPath(const PathEntry& endEntry,
            const std::vector<PathEntry>& pathEntries,
            const std::vector<TripStopIndex>& tsIdx) const;
    static uint64_t pathFingerprint(const Path& path);

    void expandWalking(const PQEntry& cur,
            std::vector<PathEntry>& pathEntries, MinHeap& pq);
    std::optional<int32_t> findBestDeparture(const trip_id& trip, uint32_t boardServiceSec,
            int32_t curDayStart, int curDayOffset,
            int32_t earliestDeparture, int32_t latestDeparture);
    void processTripExpansion(const PQEntry& cur,
            std::vector<PathEntry>& pathEntries, MinHeap& pq,
            uint32_t baseQueryTime, const ScheduleEntry& board,
            uint32_t boardServiceSec, int32_t selectedDep,
            size_t startIdx, const std::vector<ScheduleEntry>& sched);
    void expandTransit(const PQEntry& cur,
            std::vector<PathEntry>& pathEntries, MinHeap& pq);
    std::optional<PQEntry> expandOneStep(
            MinHeap& pq,
            std::vector<PathEntry>& pathEntries,
            std::vector<uint32_t>& earliestArrival,
            uint32_t targetStation, std::vector<uint32_t>& lastWaitCache);

public:
    QuerySearch(Query& query)
        : query(query) { }

    void buildWalkingIndex();
    void findPathsByStationIds(uint32_t startStationId, uint32_t endStationId, const int maxPaths = 3);
    static uint32_t applyRealtimeDelay(uint32_t arrivalTime, int32_t delaySeconds);
};

#endif
