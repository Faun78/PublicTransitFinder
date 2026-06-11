# MHD Finder - Public Transport Route Optimizer

MHD Finder is a library for finding routes in public transport networks.

- Loads timetables from GTFS format
- Finds optimal routes between two stops
- Takes into account:
  - Transfers between lines
  - Walking transfers between stops
  - Maximum number of transfers and walking distance
  - Real-time delays (online mode)

### Implementation Status

- Offline mode: Local search without internet
- Online mode: Integration with Golemio API for real-time delays - works only with Golemio API
  - Each GTFS implementation is unique, but to set delays for a specific GTFS you only need to implement `fetchRealtimeDelays()` for the specific `trip_id` from PID GTFS in format `<line>_<id1>_<id2>` - this integration has not yet been added for Spojenka, but PID GTFS is supported
- Flexible search: Different priorities (fastest time vs. fewest transfers)
  - Fewest transfers works only on small networks; on larger networks it hits the expansion limit and the search fails.

## Installation and Compilation

### Linux

```bash
git clone git@gitlab.mff.cuni.cz:teaching/nprg041/2025-26/zavoral/sklenav1.git
cd sklenav1/project

mkdir -p build
cd build

cmake ..
make -j$(nproc)

ls -la mhd_finder memory_stress csv_test
```

### Windows (MSVC)

```bash
git clone git@gitlab.mff.cuni.cz:teaching/nprg041/2025-26/zavoral/sklenav1.git
cd sklenav1\project

mkdir build
cd build

cmake ..
cmake --build . --config Release --parallel

ls mhd_finder.exe memory_stress.exe csv_test.exe
```

## Running

Set `.env` variables for GTFS data and API key for Golemio according to the template in [.example.env](.example.env).
If you want to test with online mode, create a key for the online version on [Golemio](https://api.golemio.cz/api-keys/auth/sign-in)
and put it in the `.env` file as `GOLEMIO_API_KEY=your_key`.

### Basic Usage

```bash
# Offline search with default time (current time and location)
./mhd_finder PID_GTFS

# With specific time (HH:MM format)
./mhd_finder PID_GTFS 14:30

# With specific date and time (YYYY-MM-DD HH:MM)
./mhd_finder PID_GTFS 2026-05-17 14:30
```

### Example

```bash
$ ./mhd_finder PID_GTFS
MHD_NET-INFO: Loading GTFS from /home/faun78/gits/zimni-2025/sklenav1/project/build/../PID_GTFS/
MHD_NET-INFO: Loaded 7908 stations, 82087 trips, 870 lines.
Stations: 7908
Trips: 82087
Lines: 870
MHD_NET-INFO: Looking up path from 536 to 31023
=== Path 1 === (6 stops)
1. Arrive at Pelc Tyrolka @ 16:57 -> WALK 10 min to Praha-Holešovice @ 17:07
2. Arrive at Praha-Holešovice @ 17:07 -> Wait 3 min -> BOARD Line C @ 17:11 (arrive Florenc @ 17:14)
3. Arrive at Florenc @ 17:14 -> Wait 4 min (transfer) -> BOARD Line B @ 17:18 (arrive Černý Most @ 17:35)
...
```

## Technical Details

See [Implementation.md](./docs//Implementation.md) for a description of data structures and algorithms.

#### Network
- Loads GTFS data (stations, trips, timetables)
- Builds indexes for fast search (tripStopIndex, nextTripScheduleIndex)
- Manages walking index for walking transfers
- Integrates real-time delays from Golemio API

#### Query
- Implements Dijkstra's algorithm with priority queue
- Finds the most optimal routes between two stations
- Supports filtering by maximum number of transfers and distance

#### Path and PathLeg
- `Path`: Complete route from start to destination
- `PathLeg`: Individual route segment (transfer, trip, walk)

#### MHDCore
- Wrapper API for easier work with Query and Network
- Methods for `loadGTFS()`, `newQuery()`, `lookUp()`, `exportPathsToCsv()`
- Returns `LookupResult` which can be further processed

## Testing

For tests, I expect that you have previously downloaded and extracted GTFS data, or mhd_finder will download it from the Spojenka or PID websites. Example Linux test run:

```bash
# Compile tests
cd build
cmake ..
make -j$(nproc)

# Run memory test - for memory leaks in RSS and virtual memory, run on Linux - Windows requires an additional library
# It searches between random stations and random times so it doesn't always find a path
./memory_stress 50 PID_GTFS

# Run CSV export test
./csv_test PID_GTFS
```

## Dependencies

The project uses FetchContent for automatic downloads:
- **nlohmann/json** (v3.11.3) - JSON parsing
- **cpp-httplib** (v0.45.0) - HTTP client
- **libzip** - ZIP archive handling
- **OpenSSL** / **MbedTLS** - SSL/TLS support for HTTPS requests

All dependencies are either downloaded automatically or searched for in the system.
