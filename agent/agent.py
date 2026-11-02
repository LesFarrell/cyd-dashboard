#!/usr/bin/env python3
"""PC agent for the CYD dashboard.

Collects CPU / GPU / RAM stats, local weather (Open-Meteo) and RSS news
headlines, then pushes them to the ESP32 over your LAN.
"""

import argparse
import html
import json
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
DEFAULT_FEED = "http://feeds.bbci.co.uk/news/rss.xml"
CONFIG_FILE = Path(__file__).with_name("agent.json")
STATE = {"host": None, "lock": None}


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
    payload = {
        "cpu": psutil.cpu_percent(None),
        "cpu_temp": cpu_temperature(),
        "ram": vm.percent,
        "ram_used": round(vm.used / 2**30, 1),
        "ram_total": round(vm.total / 2**30, 1),
        "host": platform.node(),
        "host_uptime": int(time.time() - psutil.boot_time()),
    }
    if not no_gpu:
        payload.update(gpu_stats())
    return payload


def fetch_weather(lat, lon, units):
    params = {
        "latitude": lat,
        "longitude": lon,
        "current": "temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m",
        "timezone": "auto",
    }
    if units == "imperial":
        params["temperature_unit"] = "fahrenheit"
        params["wind_speed_unit"] = "mph"
    r = requests.get("https://api.open-meteo.com/v1/forecast", params=params, timeout=10)
    r.raise_for_status()
    cur = r.json()["current"]
    return {
        "temp": cur["temperature_2m"],
        "feels": cur["apparent_temperature"],
        "humidity": cur["relative_humidity_2m"],
        "wind": cur["wind_speed_10m"],
        "code": cur["weather_code"],
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
            weather = fetch_weather(dev.get("lat", 51.5072), dev.get("lon", -0.1276), dev.get("units", "metric"))
            titles, source = fetch_news(dev.get("feed") or DEFAULT_FEED)
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
        titles, source = fetch_news(dev.get("feed") or DEFAULT_FEED)
        payload = dict(weather)
        payload.update({"city": dev.get("city", ""), "units": dev.get("units", "metric"), "headlines": titles, "source": source})
        session.post(f"http://{host}/api/feed", json=payload, timeout=5)
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
