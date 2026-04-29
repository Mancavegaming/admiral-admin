#!/usr/bin/env python3
"""spawn_item.py — server-side item spawn for Windrose dedicated servers.

Stops the server, inserts an FR5BLItem entry into a target chest's inventory,
restarts the server. Operates on the world RocksDB at:
  R5/Saved/SaveProfiles/Default/RocksDB/0.10.0/Worlds/{islandId}/

Requirements: rocksdict, pymongo (`pip install rocksdict pymongo`).

Usage examples:
  # By chest key (32-char hex), spawn 999 wood
  python spawn_item.py --chest A19BDE2BCE1847084BB948AC1B79646A \
    --item-substr Wood_T01 --count 999

  # By marker item: chest's slot 0 must contain "ScallopShell"
  python spawn_item.py --marker ScallopShell --item-path /R5BusinessRules/.../DA_DID_Resource_Wood_T01.DA_DID_Resource_Wood_T01 --count 999

  # Dry-run: resolve and print, do not write or restart
  python spawn_item.py --chest A19BDE2BCE... --item-substr Wood --count 999 --dry-run

Environment overrides:
  WR_GAME_DIR    — server install dir (default: standard Steam path)
  WR_WORLD_ID    — 32-char hex world ID (default: 622DBF23C4884997D28DE50FDFABC064)
  WR_PANEL_URL   — AdmiralsPanel status URL (default: http://127.0.0.1:8790/api/status)
"""
import argparse
import os
import re
import subprocess
import sys
import time
import urllib.request
import uuid
from pathlib import Path

import bson  # noqa  (pymongo)
from rocksdict import AccessType, DBCompressionType, Options, Rdict


# --- Config -----------------------------------------------------------------

GAME_DIR = Path(os.environ.get(
    "WR_GAME_DIR",
    r"C:\Program Files (x86)\Steam\steamapps\common\Windrose Dedicated Server",
))
SERVER_EXE = GAME_DIR / "R5" / "Binaries" / "Win64" / "WindroseServer-Win64-Shipping.exe"
WORLD_ID = os.environ.get("WR_WORLD_ID", "622DBF23C4884997D28DE50FDFABC064")
DB_PATH = GAME_DIR / "R5" / "Saved" / "SaveProfiles" / "Default" / "RocksDB" / "0.10.0" / "Worlds" / WORLD_ID
PANEL_URL = os.environ.get("WR_PANEL_URL", "http://127.0.0.1:8790/api/status")
SERVER_PROCESS_NAME = "WindroseServer-Win64-Shipping.exe"


# --- RocksDB helpers (no compression — Windrose's RocksDB build has none) ---

def make_opt() -> Options:
    o = Options(raw_mode=True)
    o.set_compression_type(DBCompressionType.none())
    return o


def open_db(read_only: bool):
    cfs = Rdict.list_cf(str(DB_PATH))
    cf_opts = {n: make_opt() for n in cfs}
    if read_only:
        return Rdict(str(DB_PATH), options=make_opt(),
                     column_families=cf_opts, access_type=AccessType.read_only())
    return Rdict(str(DB_PATH), options=make_opt(), column_families=cf_opts)


def cleanup_artifacts():
    """rocksdict drops a small config file; remove it after writes."""
    for f in (DB_PATH / "rocksdict-config.json",):
        if f.exists():
            f.unlink()


# --- Server lifecycle -------------------------------------------------------

def is_lock_free() -> bool:
    """Whether the RocksDB LOCK file is openable — true source of truth for
    'is the server holding the DB?'. Independent of process listing quirks."""
    try:
        with open(DB_PATH / "LOCK", "rb"):
            return True
    except (PermissionError, OSError):
        return False


def stop_server(timeout: int = 10) -> bool:
    """Force-kill the server if it holds the DB lock. Returns True if we
    actually had to kill something (caller uses this to decide whether to
    restart at the end)."""
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
    # CREATE_NEW_CONSOLE=0x00000010 keeps the server's log window separate.
    subprocess.Popen([str(SERVER_EXE), "-log"], cwd=str(GAME_DIR),
                     creationflags=0x00000010)


def wait_for_panel(timeout: int = 120) -> bool:
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


# --- Resolution: find target chest + target item path -----------------------

def find_chest_by_marker(db, marker_substr: str, marker_slot: int = 0):
    """Find a BuildingBlock record whose slot[marker_slot]'s ItemParams
    contains the marker substring. Returns (key_bytes, doc) or (None, None)."""
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    needle = marker_substr.encode()
    for key, value in cf.items():
        if needle not in value:
            continue
        try:
            doc = bson.decode(value)
        except Exception:
            continue
        modules = doc.get("Inventory", {}).get("Modules", [])
        if not modules:
            continue
        slots = modules[0].get("Slots", [])
        if marker_slot >= len(slots):
            continue
        ip = slots[marker_slot].get("ItemsStack", {}).get("Item", {}).get("ItemParams", "")
        if marker_substr in ip:
            return key, doc
    return None, None


def find_chest_by_key(db, hex_key: str):
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    try:
        value = cf[hex_key.encode()]
    except KeyError:
        return None, None
    try:
        return hex_key.encode(), bson.decode(value)
    except Exception:
        return None, None


_ASSET_PATH_RE = re.compile(rb"/R5BusinessRules/InventoryItems/[A-Za-z0-9/_]+\.[A-Za-z0-9_]+")


def find_item_path(db, substr: str) -> str | None:
    """Scan all BuildingBlock records for the first asset path matching substr.
    Returns full path string or None."""
    cf = db.get_column_family("R5BLActor_BuildingBlock")
    needle = substr.lower().encode()
    seen: set[bytes] = set()
    for _, value in cf.items():
        for m in _ASSET_PATH_RE.finditer(value):
            p = m.group(0)
            if p in seen:
                continue
            seen.add(p)
            if needle in p.lower():
                return p.decode()
    return None


def find_empty_slot(slots: list) -> int | None:
    for i, s in enumerate(slots):
        if s.get("ItemsStack", {}).get("Count", 0) == 0:
            return i
    return None


def insert_item(doc: dict, item_path: str, count: int) -> int:
    slots = doc["Inventory"]["Modules"][0]["Slots"]
    idx = find_empty_slot(slots)
    if idx is None:
        raise ValueError("No empty slot in target chest (all 16 slots full)")
    slot = slots[idx]
    slot["ItemsStack"]["Count"] = count
    slot["ItemsStack"]["Item"]["ItemParams"] = item_path
    slot["ItemsStack"]["Item"]["ItemId"] = uuid.uuid4().hex.upper()
    slot["ItemsStack"]["Item"]["Attributes"] = []
    slot["ItemsStack"]["Item"]["Effects"] = []
    return idx


# --- Main -------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    chest_group = ap.add_mutually_exclusive_group(required=True)
    chest_group.add_argument("--chest", help="Target chest by 32-char hex key")
    chest_group.add_argument("--marker", help="Target chest by marker item substring")
    ap.add_argument("--marker-slot", type=int, default=0,
                    help="Slot index that holds the marker item (default 0)")
    item_group = ap.add_mutually_exclusive_group(required=True)
    item_group.add_argument("--item-path",
                            help="Full asset path to spawn (must start with /R5BusinessRules/)")
    item_group.add_argument("--item-substr",
                            help="Substring of asset path; first match in DB used")
    ap.add_argument("--count", type=int, required=True, help="Stack count to spawn")
    ap.add_argument("--dry-run", action="store_true",
                    help="Resolve and print but do not write or restart")
    args = ap.parse_args()

    if not DB_PATH.is_dir():
        print(f"[spawn-item] ERROR: world DB not found at {DB_PATH}")
        return 2

    we_stopped_it = False
    if not args.dry_run:
        if is_lock_free():
            print("[spawn-item] Server already stopped")
        else:
            print("[spawn-item] Stopping server...")
            we_stopped_it = stop_server()
            if not is_lock_free():
                print("[spawn-item] ERROR: server kill issued but DB lock still held")
                return 1
            print("[spawn-item] Server stopped, DB lock free")

    try:
        db = open_db(read_only=args.dry_run)
        try:
            # Resolve target chest
            if args.chest:
                key, doc = find_chest_by_key(db, args.chest)
                if key is None:
                    print(f"[spawn-item] ERROR: chest key {args.chest!r} not found")
                    return 1
                print(f"[spawn-item] Target chest (by key): {args.chest}")
            else:
                key, doc = find_chest_by_marker(db, args.marker, args.marker_slot)
                if key is None:
                    print(f"[spawn-item] ERROR: no chest with marker {args.marker!r} "
                          f"in slot {args.marker_slot}")
                    return 1
                print(f"[spawn-item] Target chest (by marker): {key.decode()}")

            # Resolve item path
            if args.item_path:
                item_path = args.item_path
            else:
                item_path = find_item_path(db, args.item_substr)
                if not item_path:
                    print(f"[spawn-item] ERROR: no asset path matches {args.item_substr!r}")
                    return 1
            print(f"[spawn-item] Item: {item_path}")
            print(f"[spawn-item] Count: {args.count}")

            # Mutate
            try:
                slot_idx = insert_item(doc, item_path, args.count)
            except ValueError as e:
                print(f"[spawn-item] ERROR: {e}")
                return 1

            if args.dry_run:
                print(f"[spawn-item] [DRY RUN] Would insert into slot {slot_idx}")
                return 0

            cf = db.get_column_family("R5BLActor_BuildingBlock")
            cf[key] = bson.encode(doc)
            print(f"[spawn-item] Wrote modified record (slot {slot_idx} populated)")
        finally:
            db.close()
            cleanup_artifacts()

        if we_stopped_it and not args.dry_run:
            print("[spawn-item] Restarting server...")
            start_server()
            if wait_for_panel():
                print("[spawn-item] Server back up. Item is in chest.")
            else:
                print("[spawn-item] WARNING: panel did not come up within timeout")

        return 0
    except Exception as e:
        print(f"[spawn-item] ERROR: {type(e).__name__}: {e}")
        if we_stopped_it and not args.dry_run:
            print("[spawn-item] Attempting server restart anyway...")
            start_server()
            wait_for_panel()
        return 1


if __name__ == "__main__":
    sys.exit(main())
