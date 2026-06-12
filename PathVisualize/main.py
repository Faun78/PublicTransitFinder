import pandas as pd
import geopandas as gpd
import matplotlib.pyplot as plt
import contextily as cx
import glob
import imageio.v2 as imageio
import os
from shapely.geometry import LineString, Point

def interpolate_position(df, target_time):
    """Interpoluje pozici mezi dvěma body podle času"""
    if target_time <= df.index[0]:
        return df.iloc[0][['latitude', 'longitude']]
    if target_time >= df.index[-1]:
        return df.iloc[-1][['latitude', 'longitude']]
    
    # Najdeme segment, ve kterém se čas nachází
    for i in range(len(df) - 1):
        t1, t2 = df.index[i], df.index[i + 1]
        if t1 <= target_time <= t2:
            row1, row2 = df.iloc[i], df.iloc[i + 1]
            ratio = (target_time - t1) / (t2 - t1)
            return pd.Series({
                'latitude': row1.latitude + ratio * (row2.latitude - row1.latitude),
                'longitude': row1.longitude + ratio * (row2.longitude - row1.longitude)
            })
    return None

def load_data():
    """Načte data všech týmů"""
    teams = {}
    for file in glob.glob('data/*.csv'):
        df = pd.read_csv(file, encoding='latin1')
        team_name = os.path.basename(file).replace('.csv', '')
        df['timestamp'] = pd.to_datetime(df['arrival_date'] + ' ' + df['arrival_time'])
        teams[team_name] = df.set_index('timestamp').sort_index()
    return teams

def get_map_bounds(teams, buffer=8000):
    """Spočítá pevné hranice mapy"""
    all_coords = []
    for df in teams.values():
        for _, row in df.iterrows():
            all_coords.append((row.longitude, row.latitude))
    
    gdf = gpd.GeoDataFrame(geometry=gpd.points_from_xy(
        [c[0] for c in all_coords], [c[1] for c in all_coords]
    ), crs="EPSG:4326").to_crs(epsg=3857)
    
    bounds = gdf.total_bounds
    return (bounds[0] - buffer, bounds[2] + buffer), (bounds[1] - buffer, bounds[3] + buffer)

def create_frame(teams, current_time, bounds, colors, title, map_source):
    """Vytvoří jeden snímek animace"""
    fig, ax = plt.subplots(figsize=(14, 10), dpi=100)
    
    # Neviditelné body pro zachování rozsahu
    for x in [bounds[0][0], bounds[0][1]]:
        for y in [bounds[1][0], bounds[1][1]]:
            ax.scatter(x, y, s=1, alpha=0, color='white', zorder=-10)
    
    # Vykreslení týmů
    for team, df in teams.items():
        if current_time < df.index[0]:
            continue
            
        color = colors[team]
        
        # Cesta do aktuálního času
        historical = df[df.index <= current_time]
        if len(historical) > 0:
            points = [(row.longitude, row.latitude) for _, row in historical.iterrows()]
            
            # Přidáme aktuální interpolovanou pozici
            pos = interpolate_position(df, current_time)
            if pos is not None:
                points.append((pos.longitude, pos.latitude))
            
            if len(points) >= 2:
                line = gpd.GeoSeries([LineString(points)], crs="EPSG:4326").to_crs(epsg=3857)
                line.plot(ax=ax, color=color, linewidth=3, alpha=0.9, zorder=2)
        
        # Aktuální pozice
        if pos is not None:
            point = gpd.GeoSeries([Point(pos.longitude, pos.latitude)], crs="EPSG:4326").to_crs(epsg=3857)
            ax.scatter(point.x[0], point.y[0], c=[color], s=200, 
                      edgecolors='black', linewidth=2, zorder=3)
    
    # Mapa a vzhled
    cx.add_basemap(ax, source=map_source, alpha=0.9)
    ax.set_xlim(bounds[0])
    ax.set_ylim(bounds[1])
    ax.set_title(f"{title}\n{current_time.strftime('%Y-%m-%d %H:%M')}", 
                fontsize=16, pad=20, fontweight='bold')
    ax.axis('off')
    
    return fig

def create_map_gif(sampling='15min', map_type='light', frame_duration=0.3, 
                   title_text="Pozice týmů", zoom_buffer=8000):
    """Hlavní funkce pro vytvoření GIFu"""
    
    # Načtení dat
    print("Načítám data...")
    teams = load_data()
    print(f"Načteno {len(teams)} týmů")
    
    # Časové rozmezí
    all_times = [t for df in teams.values() for t in df.index]
    time_steps = pd.date_range(start=min(all_times), end=max(all_times), freq=sampling)
    print(f"Generuji {len(time_steps)} snímků (interval {sampling})")
    
    # Mapové nastavení
    bounds = get_map_bounds(teams, zoom_buffer)
    map_sources = {
        'light': cx.providers.CartoDB.Positron,
        'dark': cx.providers.CartoDB.DarkMatter,
        'osm': cx.providers.OpenStreetMap.Mapnik,
    }
    
    # Barvy pro týmy
    colors = {team: plt.cm.tab20(i % 20) for i, team in enumerate(sorted(teams.keys()))}
    
    # Generování snímků
    frames = []
    for i, t in enumerate(time_steps):
        if i % 20 == 0:
            print(f"  Snímek {i+1}/{len(time_steps)} - {t.strftime('%H:%M')}")
        
        fig = create_frame(teams, t, bounds, colors, title_text, map_sources.get(map_type))
        fname = f"frame_{i:03d}.png"
        plt.savefig(fname, dpi=100, bbox_inches='tight', pad_inches=0.1)
        plt.close()
        frames.append(fname)
    
    # Tvorba GIFu
    print("Sestavuji GIF...")
    images = [imageio.imread(f) for f in frames]
    output = f'cesta_tymu_{sampling}_{map_type}.gif'
    imageio.mimsave(output, images, duration=frame_duration, loop=0)
    
    # Úklid
    for f in frames:
        os.remove(f)
    
    print(f"\nHotovo! Soubor '{output}'")
    print(f"Snímků: {len(frames)}, Délka: {len(frames) * frame_duration:.1f}s")

if __name__ == "__main__":
    create_map_gif(
        sampling='1min',
        map_type='osm', 
        frame_duration=2,
        title_text="Cesta týmů Impakt Praha Holešovice",
        zoom_buffer=80000
    )