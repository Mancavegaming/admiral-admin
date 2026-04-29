#!/usr/bin/env python3
"""sidecar.py — long-running watcher for AdmiralsPanel spawn requests.

The panel cannot orchestrate its own server restart (it lives inside the
WindroseServer process). This sidecar is the out-of-process worker:

  1. Watches  admiralspanel_data/rcon/spawn/  for  req_<id>.json  files
  2. For each new request:
     - Writes status_<id>.json (queued -> stopping_server -> writing_db ->
       restarting_server -> done | failed)
     - Stops the server (idempotent)
     - Opens RocksDB with compression=none, mutates target chest, writes back
     - Restarts the server, waits for panel
     - Removes the req_ file (status file is kept for ~1 hour for the UI)
  3. Loops forever. Crash-safe via Scheduled Task auto-restart (Phase 3).

Run manually for testing:
  python sidecar.py

Run as a Scheduled Task once Phase 3 install.ps1 is wired.
"""
import json
import os
import re
import subprocess
import sys
import time
import traceback
import urllib.request
import uuid
from pathlib import Path

import bson  # noqa: from pymongo
from rocksdict import AccessType, DBCompressionType, Options, Rdict


# --- Config -----------------------------------------------------------------

GAME_DIR = Path(os.environ.get(
    "WR_GAME_DIR",
    r"C:\Program Files (x86)\Steam\steamapps\common\Windrose Dedicated Server",
))
SERVER_EXE  = GAME_DIR / "R5" / "Binaries" / "Win64" / "WindroseServer-Win64-Shipping.exe"
WORLD_ID    = os.environ.get("WR_WORLD_ID", "622DBF23C4884997D28DE50FDFABC064")
DB_PATH     = GAME_DIR / "R5" / "Saved" / "SaveProfiles" / "Default" / "RocksDB" / "0.10.0" / "Worlds" / WORLD_ID
SPOOL_DIR   = GAME_DIR / "admiralspanel_data" / "rcon" / "spawn"
LOG_DIR     = GAME_DIR / "admiralspanel_data" / "spawn-sidecar-logs"
PANEL_URL   = os.environ.get("WR_PANEL_URL", "http://127.0.0.1:8790/api/status")
SERVER_PROCESS_NAME = "WindroseServer-Win64-Shipping.exe"
POLL_INTERVAL_S = 1.0

# Status file retention so the panel UI can show recent results.
STATUS_RETENTION_S = 60 * 60   # 1 hour


# --- RocksDB helpers --------------------------------------------------------

def make_opt() -> Options:
    o = Options(raw_mode=True)
    o.set_compression_type(DBCompressionType.none())
    return o


def open_db(read_only: bool = False):
    cfs = Rdict.list_cf(str(DB_PATH))
    cf_opts = {n: make_opt() for n in cfs}
    if read_only:
        return Rdict(str(DB_PATH), options=make_opt(),
                     column_families=cf_opts, access_type=AccessType.read_only())
    return Rdict(str(DB_PATH), options=make_opt(), column_families=cf_opts)


def cleanup_artifacts():
    cfg_file = DB_PATH / "rocksdict-config.json"
    if cfg_file.exists():
        try: cfg_file.unlink()
        except OSError: pass


# --- Server lifecycle -------------------------------------------------------

def is_lock_free() -> bool:
    try:
        with open(DB_PATH / "LOCK", "rb"):
            return True
    except (PermissionError, OSError):
        return False


def stop_server(timeout: int = 15) -> bool:
    """Returns True iff we actually killed something (caller decides restart)."""
    if is_lock_free():
        return False
    subprocess.run(["taskkill", "/F", "/IM", SERVER_PROCESS_NAME],
                   capture_output=True, text=True)
    for _ in range(timeout):
        if is_lock_free():
            return True
        time.sleep(1)
    return is_lock_free()


def start_server():
    subprocess.Popen([str(SERVER_EXE), "-log"], cwd=str(GAME_DIR),
                     creationflags=0x00000010)  # CREATE_NEW_CONSOLE


def wait_for_panel(timeout: int = 180) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(PANEL_URL, timeout=2) as r:
                if r.status == 200:
                    return True
        except Exception:
            pass
        time.sleep(2)
    return False


# --- Resolution -------------------------------------------------------------

_ASSET_PATH_RE = re.compile(rb"/R5BusinessRules/InventoryItems/[A-Za-z0-9/_]+\.[A-Za-z0-9_]+")


def find_chest_by_key(db, hex_key: str):
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    try:
        return hex_key.encode(), bson.decode(cf[hex_key.encode()])
    except (KeyError, Exception):
        return None, None


def find_chest_by_marker(db, marker_substr: str, marker_slot: int = 0):
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    needle = marker_substr.encode()
    for key, value in cf.items():
        if needle not in value:
            continue
        try: doc = bson.decode(value)
        except Exception: continue
        modules = doc.get("Inventory", {}).get("Modules", [])
        if not modules: continue
        slots = modules[0].get("Slots", [])
        if marker_slot >= len(slots): continue
        ip = slots[marker_slot].get("ItemsStack", {}).get("Item", {}).get("ItemParams", "")
        if marker_substr in ip:
            return key, doc
    return None, None


def find_item_path(db, substr: str):
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    needle = substr.lower().encode()
    seen: set[bytes] = set()
    for _, value in cf.items():
        for m in _ASSET_PATH_RE.finditer(value):
            p = m.group(0)
            if p in seen: continue
            seen.add(p)
            if needle in p.lower():
                return p.decode()
    return None


def find_empty_slot(slots):
    for i, s in enumerate(slots):
        if s.get("ItemsStack", {}).get("Count", 0) == 0:
            return i
    return None


# --- Status writes ----------------------------------------------------------

def write_status(req_id: str, **fields):
    fields["id"] = req_id
    fields["ts"] = int(time.time() * 1000)
    path = SPOOL_DIR / f"status_{req_id}.json"
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(fields), encoding="utf-8")
    tmp.replace(path)


# --- Per-request worker -----------------------------------------------------

def process_request(req_path: Path):
    log(f"processing {req_path.name}")
    try:
        req = json.loads(req_path.read_text(encoding="utf-8"))
    except Exception as e:
        log(f"  [ERROR] cannot parse {req_path.name}: {e}")
        try: req_path.unlink()
        except OSError: pass
        return

    req_id = req.get("id", req_path.stem.replace("req_", ""))
    we_stopped = False

    try:
        write_status(req_id, status="stopping_server")

        if not is_lock_free():
            log(f"  stopping server")
            we_stopped = stop_server()
            if not is_lock_free():
                raise RuntimeError("server kill issued but DB lock still held")

        write_status(req_id, status="writing_db")
        log(f"  opening DB")
        db = open_db(read_only=False)
        try:
            # Resolve chest
            if req.get("chest"):
                key, doc = find_chest_by_key(db, req["chest"])
                if key is None:
                    raise RuntimeError(f"chest key {req['chest']!r} not found")
                chest_label = req["chest"]
            elif req.get("marker"):
                key, doc = find_chest_by_marker(db, req["marker"], req.get("marker_slot", 0))
                if key is None:
                    raise RuntimeError(f"no chest with marker {req['marker']!r}")
                chest_label = key.decode()
            else:
                raise RuntimeError("request has no chest or marker")

            # Resolve item path
            if req.get("item_path"):
                item_path = req["item_path"]
            elif req.get("item_substr"):
                item_path = find_item_path(db, req["item_substr"])
                if not item_path:
                    raise RuntimeError(f"no asset path matches {req['item_substr']!r}")
            else:
                raise RuntimeError("request has no item_path or item_substr")

            count = int(req.get("count", 0))
            if count <= 0:
                raise RuntimeError("count must be positive")

            # Find empty slot + populate
            slots = doc["Inventory"]["Modules"][0]["Slots"]
            slot_idx = find_empty_slot(slots)
            if slot_idx is None:
                raise RuntimeError("target chest has no empty slot")
            slot = slots[slot_idx]
            slot["ItemsStack"]["Count"] = count
            slot["ItemsStack"]["Item"]["ItemParams"] = item_path
            slot["ItemsStack"]["Item"]["ItemId"] = uuid.uuid4().hex.upper()
            slot["ItemsStack"]["Item"]["Attributes"] = []
            slot["ItemsStack"]["Item"]["Effects"] = []

            cf = db.get_column_family("R5BLActor_BuildingBlock")
            cf[key] = bson.encode(doc)
            log(f"  wrote chest={chest_label} slot={slot_idx} item={item_path} count={count}")
        finally:
            db.close()
            cleanup_artifacts()

        if we_stopped:
            write_status(req_id, status="restarting_server")
            log(f"  restarting server")
            start_server()
            if not wait_for_panel():
                raise RuntimeError("panel did not come up within timeout after restart")

        write_status(req_id,
                     status="done",
                     chest=chest_label,
                     item=item_path,
                     count=count,
                     slot=slot_idx)
        log(f"  DONE id={req_id}")

    except Exception as e:
        tb = traceback.format_exc()
        log(f"  [FAILED] {e!r}\n{tb}")
        write_status(req_id, status="failed", error=str(e))
        # If we stopped the server, attempt restart anyway so we don't leave it down.
        if we_stopped and not is_lock_free():
            pass  # already restarted (lock held by new server)
        elif we_stopped:
            log(f"  attempting recovery server restart")
            start_server()
            wait_for_panel()
    finally:
        try: req_path.unlink()
        except OSError: pass


# --- Cleanup of old status files --------------------------------------------

def reap_old_status():
    cutoff = time.time() - STATUS_RETENTION_S
    for f in SPOOL_DIR.glob("status_*.json"):
        try:
            if f.stat().st_mtime < cutoff:
                f.unlink()
        except OSError:
            pass


# --- Main loop --------------------------------------------------------------

def log(msg: str):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)
    try:
        log_path = LOG_DIR / f"sidecar-{time.strftime('%Y%m%d')}.log"
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def acquire_singleton_lock():
    """Prevent multiple sidecar instances. Uses a file we hold open + lock for
    process lifetime. Returns the file handle (keep alive)."""
    import msvcrt
    lock_path = GAME_DIR / "admiralspanel_data" / "spawn-sidecar.lock"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_handle = open(lock_path, "w")
    try:
        msvcrt.locking(lock_handle.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        log(f"another sidecar instance is running; exiting")
        lock_handle.close()
        sys.exit(0)
    lock_handle.write(str(os.getpid()))
    lock_handle.flush()
    return lock_handle  # caller must keep alive for process lifetime


def main():
    SPOOL_DIR.mkdir(parents=True, exist_ok=True)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    _lock = acquire_singleton_lock()  # keep handle alive (intentional unused)
    log(f"sidecar starting; watching {SPOOL_DIR}")

    last_reap = 0.0
    while True:
        try:
            # Process all pending request files (sorted oldest first by mtime)
            reqs = sorted(SPOOL_DIR.glob("req_*.json"), key=lambda p: p.stat().st_mtime)
            for r in reqs:
                process_request(r)

            # Periodic cleanup of old status files
            if time.time() - last_reap > 60:
                reap_old_status()
                last_reap = time.time()
        except Exception as e:
            log(f"[loop error] {e!r}\n{traceback.format_exc()}")

        time.sleep(POLL_INTERVAL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
