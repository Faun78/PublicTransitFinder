# MHD Finder - Optimalizátor tras v hromadné dopravě

MHD Finder je knihovna na vyhledávání tras v MHD

- Načítá jízdní řády z GTFS formátu
- Hledá optimální trasy mezi dvěma zastávkami
- Zohledňuje:
  - Přestupy mezi linkami
  - Pěší přesuny mezi zastávkami
  - Maximální počet přestupů a vzdálenost
  - Aktuální zpoždění (online režim)

### Stav implementace

- Offline režim: Lokální vyhledávání bez internetu
- Online režim: Integrace s Golemio API pro real-time zpoždění - funguje pouze s Golemio API
  - Každá GTFS implementace je v něčem unikátní ale pro nastavení zpoždění pro konkrétní GTFS je potřeba pouze implementovat `fetchRealtimeDelays()` pro specifický `trip_id` z PID GTFS ve formátu `<line>_<id1>_<id2>` - tato integrace zatím nebyla doplněna pro spojenku ale PID GTFS jsou podporovány
- Flexibilní hledání: Různé priority (nejrychlejší čas vs. nejméně přestupů)
  - nejméně přestupů funguje jen na malé sítě, na větší síti se hitne limit pro maximální počet expanzí a hledání selže.

## Instalace a kompilace

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

## Spuštění

Nastavení .env proměnných pro GTFS data a API klíč pro Golemio dle vzoru v [.example.env](../.example.env).
Pokud chcete testovat s online režimem vytvořte si klíč pro online verzi na [Golemio](https://api.golemio.cz/api-keys/auth/sign-in)
a vložte ho do .env souboru jako `GOLEMIO_API_KEY=váš_klíč`.

### Základní použití

```bash
# Offline vyhledávání s výchozím časem (aktuální čas a místo)
./mhd_finder PID_GTFS

# S konkrétním časem (HH:MM formát)
./mhd_finder PID_GTFS 14:30

# S konkrétním datem a časem (YYYY-MM-DD HH:MM)
./mhd_finder PID_GTFS 2026-05-17 14:30
```

### Příklad

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
## Technické detaily

Viz [Implementation.md](Implementation.md) pro popis datových struktur a algoritmů.

#### Network
- Načítá GTFS data (stanice, spoje, jízdní řády)
- Buduje indexy pro rychlé vyhledávání (tripStopIndex, nextTripScheduleIndex)
- Spravuje walking index pro pěší přesuny
- Integruje real-time zpoždění z Golemio API

#### Query
- Implementuje Dijkstrův algoritmus s prioritní frontou
- Vyhledává nejoptimálnější trasy mezi dvěma stanicemi
- Podporuje filtrování podle maximálního počtu přestupů a vzdálenosti

#### Path a PathLeg
- `Path`: Kompletní trasa od startu do cíle
- `PathLeg`: Jednotlivý segment trasy (přestup, jízda, pěšinka)

#### MHDCore
- Wrapper API pro jednodušší práci s Query a Network
- Metody pro `loadGTFS()`, `newQuery()`, `lookUp()`, `exportPathsToCsv()`
- Vrací `LookupResult` se kterým lze dále pracovat

## Testování

Pro testy očekávám, že jste si předtím stáhli a rozbalili GTFS data nebo vám je stáhne mhd_finder ze stránky Spojenky nebo PIDu. Ukázka linux spuštění testů:

```bash
# Kompilace testů
cd build
cmake ..
make -j$(nproc)

# Spuštění memory testu - pro memory leaky v rss a virtuální paměti spouštějte pod Linuxem - Windows potřebuje další knihovnu
# Hledá to mezi náhdonými stanicemi a náhodnými časy tedy ne vždy najde cestu
./memory_stress 50 PID_GTFS

# Spuštění CSV export testu
./csv_test PID_GTFS
```

## Závislosti

Projekt používá FetchContent pro automatické stažení:
- **nlohmann/json** (v3.11.3) - JSON parsing
- **cpp-httplib** (v0.45.0) - HTTP klient
- **libzip** - ZIP archive handling
- **OpenSSL** / **MbedTLS** - SSL/TLS support pro HTTPS požadavky

Všechny závislosti jsou buď staženy automaticky, nebo se vyhledají v systému.
