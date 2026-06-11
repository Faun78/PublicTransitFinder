#ifndef STATIONS_H
#define STATIONS_H
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

inline uint64_t gtfsHash(std::string_view s) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// trip_id: FNV-1a hash of the GTFS string
// line assigned at load time
struct trip_id {
    uint64_t id = 0;
    uint32_t line = 0; // line index; 0 = unresolved/walking

    bool operator==(const trip_id& o) const noexcept { return id == o.id; }
    bool operator!=(const trip_id& o) const noexcept { return id != o.id; }
    bool operator<(const trip_id& o) const noexcept { return id < o.id; }
    trip_id(uint64_t id_ = 0, uint32_t line_ = 0)
        : id(id_)
        , line(line_) { }
};

struct TripIdHash {
    size_t operator()(const trip_id& t) const noexcept {
        return static_cast<size_t>(t.id);
    }
};

using line_id = uint32_t;
static constexpr line_id kNoLine = 0;

using service_id = uint32_t;
static constexpr service_id kNoService = 0;

struct Location {
    double latitude;
    double longitude;

    Location(double lat_, double lon_)
        : latitude(lat_)
        , longitude(lon_) { }
};

using Platform = std::pair<uint32_t, Location>;

struct Station {
    uint32_t id;
    char name[50] = { };
    std::vector<line_id> lines; // line ids that serve this station
    Location location;
    std::vector<uint32_t> platforms; // platform ids

    Station(uint32_t id_, const std::string& name_, const Location& location_, uint32_t platformId = 0)
        : id(id_)
        , location(location_) {
        strncpy(name, name_.c_str(), sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (platformId != 0)
            platforms.push_back(platformId);
    }

    const uint32_t& getId() const { return id; }
    const Location& getLocation() const { return location; }
    const std::string getName() const { return std::string(name); }
    void addPlatform(const Platform& platform) {
        platforms.push_back(platform.first);
    }
    ~Station() = default;
};

#endif