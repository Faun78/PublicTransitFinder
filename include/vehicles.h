#ifndef VEHICLES_H
#define VEHICLES_H
#include "stations.h"
#include <tuple>

struct Line {
    uint32_t id;
    std::string name;
    // trip schedule entries: (time_seconds, trip_id, station_id)
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> tripSchedule;
    uint8_t lineOperations; // bitmask for days (bit 0=Monday, bit 6=Sunday)

    ~Line() = default;
};

struct Trip {
    trip_id id;
    // schedule: pairs of (station_id, arrival_time_seconds)
    std::vector<std::pair<uint32_t, uint32_t>> schedule;
    uint8_t operations; // bitmask for days of operation
    int32_t delay_seconds; // realtime delay in seconds -> used in online mode
    Location location; // current or last-known location
    std::string specialInfo; // info from GTFS

    ~Trip() = default;
};

#endif
