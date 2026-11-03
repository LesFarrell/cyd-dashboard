#!/usr/bin/env python3
"""PC agent for the CYD dashboard.

Collects CPU / GPU / RAM stats, local weather (Open-Meteo), RSS news
headlines and live Heysham tide-gauge readings, then pushes them to the
ESP32 over your LAN.
"""

import argparse
import datetime
import html
import json
import math
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import psutil
import requests

DISCOVER_PORT = 8266
DEFAULT_FEED = "\n".join(
    [
        "http://feeds.bbci.co.uk/news/rss.xml",
        "http://rss.cnn.com/rss/edition.rss",
        "https://news.google.com/rss",
    ]
)
CONFIG_FILE = Path(__file__).with_name("agent.json")
STATE = {"host": None, "lock": None, "net": None, "net_at": None}
DEFAULT_TIDE_STATION = "E73424"
TIDE_LOCATION_CACHE = {}


def load_agent_config():
    try:
        return json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_agent_config(cfg):
    try:
        CONFIG_FILE.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    except Exception:
        pass


def discover(timeout=3.0):
    """Broadcast for the CYD and return its IP."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.3)

    targets = {"255.255.255.255"}
    for addrs in psutil.net_if_addrs().values():
        for a in addrs:
            if a.family != socket.AF_INET or not a.address:
                continue
            if a.broadcast:
                targets.add(a.broadcast)
            if a.netmask:
                try:
                    ip = struct.unpack("!I", socket.inet_aton(a.address))[0]
                    mask = struct.unpack("!I", socket.inet_aton(a.netmask))[0]
                    targets.add(socket.inet_ntoa(struct.pack("!I", ip | (~mask & 0xFFFFFFFF))))
                except OSError:
                    pass

    for target in targets:
        try:
            sock.sendto(b"CYD_DISCOVER", (target, DISCOVER_PORT))
        except OSError:
            pass

    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(512)
        except socket.timeout:
            continue
        try:
            info = json.loads(data.decode("utf-8", "ignore"))
        except Exception:
            continue
        if info.get("ip"):
            sock.close()
            return info["ip"]
    sock.close()
    return None


_wmi = None


def cpu_temperature():
    """Optional: CPU package temp via LibreHardwareMonitor (pip install wmi)."""
    global _wmi
    if _wmi is None:
        try:
            import wmi

            _wmi = wmi.WMI(namespace="root/LibreHardwareMonitor")
        except Exception:
            _wmi = False
    if not _wmi:
        return None
    try:
        fallback = None
        for sensor in _wmi.Sensor():
            if sensor.SensorType != "Temperature" or sensor.Value is None:
                continue
            if "CPU Package" in sensor.Name:
                return float(sensor.Value)
            if "CPU" in sensor.Name and fallback is None:
                fallback = float(sensor.Value)
        return fallback
    except Exception:
        return None


def gpu_stats():
    if os.name == "nt":
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    else:
        flags = 0
    try:
        out = subprocess.check_output(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu",
                "--format=csv,noheader,nounits",
            ],
            timeout=3,
            text=True,
            stderr=subprocess.DEVNULL,
            creationflags=flags,
        )
        parts = [p.strip() for p in out.strip().splitlines()[0].split(",")]
        return {
            "gpu": float(parts[0]),
            "gpu_mem_used": float(parts[1]),
            "gpu_mem_total": float(parts[2]),
            "gpu_temp": float(parts[3]),
        }
    except Exception:
        return {}


def collect(no_gpu=False):
    vm = psutil.virtual_memory()
    system_root = os.environ.get("SystemDrive", "C:") + "\\" if os.name == "nt" else "/"
    disk = psutil.disk_usage(system_root)
    now = time.monotonic()
    net = psutil.net_io_counters()
    net_down_mbps = 0.0
    net_up_mbps = 0.0
    if STATE["net"] is not None and STATE["net_at"] is not None:
        elapsed = max(now - STATE["net_at"], 0.001)
        net_down_mbps = max(0.0, (net.bytes_recv - STATE["net"].bytes_recv) * 8 / elapsed / 1_000_000)
        net_up_mbps = max(0.0, (net.bytes_sent - STATE["net"].bytes_sent) * 8 / elapsed / 1_000_000)
    STATE["net"] = net
    STATE["net_at"] = now
    payload = {
        "cpu": psutil.cpu_percent(None),
        "cpu_temp": cpu_temperature(),
        "ram": vm.percent,
        "ram_used": round(vm.used / 2**30, 1),
        "ram_total": round(vm.total / 2**30, 1),
        "host": platform.node(),
        "host_uptime": int(time.time() - psutil.boot_time()),
        "disk": disk.percent,
        "disk_used": round(disk.used / 2**30, 1),
        "disk_total": round(disk.total / 2**30, 1),
        "net_down_mbps": round(net_down_mbps, 2),
        "net_up_mbps": round(net_up_mbps, 2),
    }
    if not no_gpu:
        payload.update(gpu_stats())
    return payload


def fetch_weather(lat, lon, units):
    params = {
        "latitude": lat,
        "longitude": lon,
        "current": "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,wind_gusts_10m",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset",
        "forecast_days": 4,
        "timezone": "auto",
    }
    if units == "imperial":
        params["temperature_unit"] = "fahrenheit"
        params["wind_speed_unit"] = "mph"
    r = requests.get("https://api.open-meteo.com/v1/forecast", params=params, timeout=10)
    r.raise_for_status()
    data = r.json()
    cur = data["current"]
    daily = data.get("daily", {})
    forecast = []
    for date_text, code, high, low in zip(
        daily.get("time", []),
        daily.get("weather_code", []),
        daily.get("temperature_2m_max", []),
        daily.get("temperature_2m_min", []),
    ):
        day = datetime.date.fromisoformat(date_text).strftime("%a")
        forecast.append({"day": day, "code": code, "high": high, "low": low})
    return {
        "temp": cur["temperature_2m"],
        "feels": cur["apparent_temperature"],
        "humidity": cur["relative_humidity_2m"],
        "wind": cur["wind_speed_10m"],
        "wind_direction": cur.get("wind_direction_10m", -1),
        "wind_gust": cur.get("wind_gusts_10m", -1),
        "sunrise": (daily.get("sunrise") or [""])[0][-5:],
        "sunset": (daily.get("sunset") or [""])[0][-5:],
        "code": cur["weather_code"],
        "forecast": forecast,
    }


def fetch_tide_location(station):
    if station in TIDE_LOCATION_CACHE:
        return TIDE_LOCATION_CACHE[station]
    if not re.fullmatch(r"E\d+", station or ""):
        raise ValueError("invalid Environment Agency tide station")
    url = f"https://environment.data.gov.uk/flood-monitoring/id/stations/{station}"
    r = requests.get(url, timeout=10, headers={"User-Agent": "cyd-dashboard/1.0"})
    r.raise_for_status()
    item = r.json()["items"]
    location = (float(item["lat"]), float(item["long"]), item.get("label") or station)
    TIDE_LOCATION_CACHE[station] = location
    return location


def moon_phase(at=None):
    """Approximate lunar phase name and illuminated percentage."""
    at = at or datetime.datetime.now(datetime.timezone.utc)
    known_new_moon = datetime.datetime(2000, 1, 6, 18, 14, tzinfo=datetime.timezone.utc)
    cycle = ((at - known_new_moon).total_seconds() / 86400 / 29.53058867) % 1
    illumination = round((1 - math.cos(2 * math.pi * cycle)) * 50)
    names = [
        "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
        "Full moon", "Waning gibbous", "Last quarter", "Waning crescent",
    ]
    return names[int((cycle * 8) + 0.5) % 8], illumination


def fetch_marine(lat, lon, station_name, units):
    wind_params = {
        "latitude": lat,
        "longitude": lon,
        "current": "wind_speed_10m,wind_direction_10m,wind_gusts_10m",
        "daily": "sunrise,sunset",
        "forecast_days": 1,
        "timezone": "auto",
    }
    if units == "imperial":
        wind_params["wind_speed_unit"] = "mph"
    weather = requests.get(
        "https://api.open-meteo.com/v1/forecast", params=wind_params, timeout=10
    )
    weather.raise_for_status()
    weather_data = weather.json()

    marine_params = {
        "latitude": lat,
        "longitude": lon,
        "current": "wave_height,wave_period,sea_surface_temperature,ocean_current_velocity,ocean_current_direction",
        "timezone": "auto",
        "cell_selection": "sea",
    }
    marine = requests.get(
        "https://marine-api.open-meteo.com/v1/marine", params=marine_params, timeout=15
    )
    marine.raise_for_status()
    sea = marine.json().get("current", {})
    wind = weather_data.get("current", {})
    daily = weather_data.get("daily", {})
    phase_name, illumination = moon_phase()
    return {
        "station": station_name,
        "wind": wind.get("wind_speed_10m", -1),
        "wind_direction": wind.get("wind_direction_10m", -1),
        "wind_gust": wind.get("wind_gusts_10m", -1),
        "wind_unit": "mph" if units == "imperial" else "km/h",
        "wave_height": sea.get("wave_height", -1),
        "wave_period": sea.get("wave_period", -1),
        "sea_temp": sea.get("sea_surface_temperature", -100),
        "current_speed": sea.get("ocean_current_velocity", -1),
        "current_direction": sea.get("ocean_current_direction", -1),
        "sunrise": (daily.get("sunrise") or [""])[0][-5:],
        "sunset": (daily.get("sunset") or [""])[0][-5:],
        "moon": phase_name,
        "moon_illumination": illumination,
    }


def find_tide_events(readings, max_events=6):
    """Find well-separated high and low turning points in gauge readings."""
    if len(readings) < 9:
        return []

    # A small moving average prevents tiny gauge fluctuations near slack water
    # from producing several apparent highs or lows.
    smooth = []
    for i in range(len(readings)):
        values = [value for _, value in readings[max(0, i - 1) : min(len(readings), i + 2)]]
        smooth.append(sum(values) / len(values))

    radius = 3  # 45 minutes for the normal 15-minute gauge interval
    candidates = []
    for i in range(radius, len(readings) - radius):
        window = smooth[i - radius : i + radius + 1]
        value = smooth[i]
        edge_level = (smooth[i - radius] + smooth[i + radius]) / 2
        if value == max(window) and value - edge_level >= 0.08:
            candidates.append((i, "H"))
        elif value == min(window) and edge_level - value >= 0.08:
            candidates.append((i, "L"))

    events = []
    minimum_gap = datetime.timedelta(hours=2)
    for index, kind in candidates:
        event = (readings[index][0], readings[index][1], kind)
        if not events:
            events.append(event)
            continue
        if kind == events[-1][2]:
            better = event[1] > events[-1][1] if kind == "H" else event[1] < events[-1][1]
            if better:
                events[-1] = event
            continue
        if event[0] - events[-1][0] < minimum_gap:
            continue
        events.append(event)

    return events[-max_events:]


def fetch_tide(station=DEFAULT_TIDE_STATION, station_name="Heysham", max_points=49):
    """Return the latest 24 hours from an EA local-datum tide gauge."""
    if not re.fullmatch(r"E\d+", station or ""):
        raise ValueError("invalid Environment Agency tide station")
    measure = f"{station}-level-tidal_level-Mean-15_min-m"
    now = datetime.datetime.now(datetime.timezone.utc)
    since = now - datetime.timedelta(hours=24, minutes=30)
    url = (
        "https://environment.data.gov.uk/flood-monitoring/id/measures/"
        f"{measure}/readings"
    )
    params = {
        "since": since.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "_sorted": "",
        "_limit": 200,
    }
    r = requests.get(url, params=params, timeout=10, headers={"User-Agent": "cyd-dashboard/1.0"})
    r.raise_for_status()

    readings = []
    for item in r.json().get("items", []):
        if "value" not in item or not item.get("dateTime"):
            continue
        stamp = datetime.datetime.fromisoformat(item["dateTime"].replace("Z", "+00:00"))
        readings.append((stamp, float(item["value"])))
    readings.sort(key=lambda item: item[0])
    if not readings:
        raise RuntimeError(f"{station_name} gauge returned no readings")

    latest_at = readings[-1][0]
    events = [
        [
            max(0, round((latest_at - stamp).total_seconds() / 60)),
            round(value, 3),
            kind,
            stamp.astimezone().strftime("%H:%M"),
        ]
        for stamp, value, kind in find_tide_events(readings)
    ]

    if len(readings) > max_points:
        indexes = [round(i * (len(readings) - 1) / (max_points - 1)) for i in range(max_points)]
        readings = [readings[i] for i in indexes]

    points = [
        [max(0, round((latest_at - stamp).total_seconds() / 60)), round(value, 3)]
        for stamp, value in readings
    ]
    return {
        "station": station_name,
        "latest_time": latest_at.astimezone().strftime("%H:%M"),
        "points": points,
        "events": events,
    }


def fetch_news(url, limit=6):
    r = requests.get(url, timeout=10, headers={"User-Agent": "cyd-dashboard/1.0"})
    r.raise_for_status()
    root = ET.fromstring(r.content)
    titles = []
    for item in root.iter("item"):
        title = item.findtext("title")
        if not title:
            continue
        title = html.unescape(re.sub(r"<[^>]+>", "", title))
        title = re.sub(r"\s+", " ", title).strip()
        if title:
            titles.append(title)
        if len(titles) >= limit:
            break
    source = root.findtext(".//channel/title") or "News"
    return titles, html.unescape(source).strip()


def feed_urls(value):
    urls = [u for u in re.split(r"[\s,]+", value or "") if u]
    return urls or [u for u in re.split(r"\s+", DEFAULT_FEED) if u]


def short_source(source):
    return re.split(r"\s+-\s+", source)[0].strip() or "News"


def fetch_news_feeds(value, limit=32):
    """Fetch every configured feed and interleave their headlines."""
    results = []
    errors = []
    for url in feed_urls(value):
        try:
            results.append(fetch_news(url, limit))
        except Exception as exc:
            errors.append(f"{url}: {exc}")
            print(f"  feed skipped ({url}): {exc}")
    if not results:
        raise RuntimeError("; ".join(errors) or "no feeds configured")

    titles = []
    seen = set()
    index = 0
    while len(titles) < limit and any(index < len(t) for t, _ in results):
        for feed_titles, _ in results:
            if index >= len(feed_titles):
                continue
            title = feed_titles[index]
            key = title.casefold()
            if key in seen:
                continue
            seen.add(key)
            titles.append(title)
            if len(titles) >= limit:
                break
        index += 1

    sources = []
    for _, source in results:
        name = short_source(source)
        if name not in sources:
            sources.append(name)
    return titles, " + ".join(sources)


def stats_loop(interval, no_gpu):
    session = requests.Session()
    misses = 0
    while True:
        host = STATE["host"]
        try:
            session.post(
                f"http://{host}/api/stats",
                json=collect(no_gpu),
                timeout=2,
            )
            if misses:
                print(f"  reconnected to {host}")
            misses = 0
        except Exception as exc:
            misses += 1
            if misses == 1:
                print(f"  cannot reach {host}: {exc}")
            if misses >= 5:
                found = discover()
                if found and found != host:
                    STATE["host"] = found
                    print(f"  CYD moved to {found}")
                misses = 0
        time.sleep(interval)


def feed_loop(feed_interval):
    session = requests.Session()
    while True:
        host = STATE["host"]
        try:
            dev = session.get(f"http://{host}/api/config", timeout=5).json()
        except Exception as exc:
            print(f"  config error: {exc}")
            time.sleep(feed_interval)
            continue
        try:
            tide_station = dev.get("tideStation", DEFAULT_TIDE_STATION)
            tide_port = dev.get("tidePort", "Heysham")
            tide = fetch_tide(
                tide_station,
                tide_port,
            )
            session.post(f"http://{host}/api/tide", json=tide, timeout=4).raise_for_status()
            print(f"  tide: {tide['station']} {tide['points'][-1][1]:.2f} m at {tide['latest_time']}")
        except Exception as exc:
            print(f"  tide error: {exc}")
        try:
            marine_lat, marine_lon, station_label = fetch_tide_location(
                dev.get("tideStation", DEFAULT_TIDE_STATION)
            )
            marine = fetch_marine(
                marine_lat,
                marine_lon,
                dev.get("tidePort") or station_label,
                dev.get("units", "metric"),
            )
            session.post(f"http://{host}/api/marine", json=marine, timeout=4).raise_for_status()
            print(f"  marine: {marine['wave_height']} m waves, {marine['sea_temp']} C sea")
        except Exception as exc:
            print(f"  marine error: {exc}")
        try:
            weather = fetch_weather(dev.get("lat", 51.5072), dev.get("lon", -0.1276), dev.get("units", "metric"))
            titles, source = fetch_news_feeds(dev.get("feed") or DEFAULT_FEED)
            payload = dict(weather)
            payload.update(
                {
                    "city": dev.get("city", ""),
                    "units": dev.get("units", "metric"),
                    "headlines": titles,
                    "source": source,
                }
            )
            session.post(f"http://{host}/api/feed", json=payload, timeout=4)
            print(
                f"  feed: {weather['temp']}°, {len(titles)} headlines from {source}"
            )
        except Exception as exc:
            print(f"  feed error: {exc}")
        time.sleep(feed_interval)


def main():
    parser = argparse.ArgumentParser(description="CYD dashboard PC agent")
    parser.add_argument("--host", help="CYD IP address (skip auto-discovery)")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds between stats packets")
    parser.add_argument("--feed-interval", type=float, default=300, help="seconds between weather/news updates")
    parser.add_argument("--no-gpu", action="store_true", help="do not query nvidia-smi")
    parser.add_argument("--discover", action="store_true", help="force a discovery scan")
    parser.add_argument("--once", action="store_true", help="send one stats packet and one feed, then exit")
    args = parser.parse_args()

    cfg = load_agent_config()
    host = args.host or cfg.get("host")

    if args.discover or not host:
        print("Looking for a CYD on the LAN...")
        found = discover()
        if found:
            host = found
            cfg["host"] = found
            save_agent_config(cfg)
            print(f"Found CYD at {host}")
        elif not host:
            print("No CYD found. Pass --host <ip> (shown on the CYD system page).")
            return 1

    STATE["host"] = host
    if cfg.get("host") != host:
        cfg["host"] = host
        save_agent_config(cfg)
    print(f"CYD dashboard agent — sending to http://{host}/api/stats")
    print(f"Host: {platform.node()}  CPU: {psutil.cpu_count(logical=True)} threads  RAM: {psutil.virtual_memory().total / 2**30:.1f} GB")
    if not args.no_gpu and not gpu_stats():
        print("  (nvidia-smi not available — GPU row will show --)")

    if args.once:
        session = requests.Session()
        session.post(f"http://{host}/api/stats", json=collect(args.no_gpu), timeout=3)
        dev = session.get(f"http://{host}/api/config", timeout=5).json()
        weather = fetch_weather(dev.get("lat", 51.5072), dev.get("lon", -0.1276), dev.get("units", "metric"))
        titles, source = fetch_news_feeds(dev.get("feed") or DEFAULT_FEED)
        payload = dict(weather)
        payload.update({"city": dev.get("city", ""), "units": dev.get("units", "metric"), "headlines": titles, "source": source})
        session.post(f"http://{host}/api/feed", json=payload, timeout=5)
        tide = fetch_tide(
            dev.get("tideStation", DEFAULT_TIDE_STATION),
            dev.get("tidePort", "Heysham"),
        )
        session.post(f"http://{host}/api/tide", json=tide, timeout=5).raise_for_status()
        marine_lat, marine_lon, station_label = fetch_tide_location(
            dev.get("tideStation", DEFAULT_TIDE_STATION)
        )
        marine = fetch_marine(
            marine_lat,
            marine_lon,
            dev.get("tidePort") or station_label,
            dev.get("units", "metric"),
        )
        session.post(f"http://{host}/api/marine", json=marine, timeout=5).raise_for_status()
        print("Sent one update.")
        return 0

    import threading

    threading.Thread(target=feed_loop, args=(args.feed_interval,), daemon=True).start()
    try:
        stats_loop(args.interval, args.no_gpu)
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
