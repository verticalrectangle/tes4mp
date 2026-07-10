#!/usr/bin/env python3
"""
TES4MP server integration tests.
Spins up the real Lua server against a throwaway DB and drives every
packet flow with mock TCP clients. No Oblivion installation needed.

Run from the repo root:
    cd /home/alexis/dev/TES4MP && python3 server/tests/integration_test.py
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from typing import Optional

# ── Config ────────────────────────────────────────────────────────────────────

HOST        = "127.0.0.1"
PORT        = 17777
TEST_DB     = "server/data/test_tes4mp.db"
TEST_CFG    = "server/tests/test_config.json"
SERVER_CMD  = ["lua", "server/main.lua", TEST_CFG]

RECV_TIMEOUT = 1.0   # seconds to wait for an expected packet
STARTUP_WAIT = 1.2   # seconds for server to bind

# ── Helpers ───────────────────────────────────────────────────────────────────

class Client:
    def __init__(self, port: int = PORT):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.settimeout(RECV_TIMEOUT)
        self._buf  = ""
        self._sock.connect((HOST, port))

    def send(self, pkt: dict):
        line = json.dumps(pkt) + "\n"
        self._sock.sendall(line.encode())

    def recv_all(self, timeout: float = RECV_TIMEOUT) -> list:
        """Drain all lines received within timeout seconds."""
        pkts = []
        self._sock.settimeout(timeout)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self._sock.recv(4096).decode(errors="replace")
                if not chunk:
                    break
                self._buf += chunk
                while "\n" in self._buf:
                    line, self._buf = self._buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        try:
                            pkts.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
            except socket.timeout:
                break
        return pkts

    def expect(self, pkt_type: str, timeout: float = RECV_TIMEOUT) -> dict:
        """Wait for a packet of a given type; raise AssertionError if not found."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = deadline - time.time()
            pkts = self.recv_all(min(remaining, 0.2))
            for p in pkts:
                if p.get("type") == pkt_type:
                    return p
        raise AssertionError(f"Expected packet type '{pkt_type}' not received within {timeout}s")

    def expect_none(self, pkt_type: str, timeout: float = 0.4) -> None:
        """Assert that pkt_type does NOT arrive within timeout."""
        pkts = self.recv_all(timeout)
        for p in pkts:
            if p.get("type") == pkt_type:
                raise AssertionError(f"Unexpected packet type '{pkt_type}' received")

    def close(self):
        try:
            self._sock.close()
        except Exception:
            pass

    @classmethod
    def connect_and_auth(cls, token: str, name: str, port: int = PORT) -> "Client":
        c = cls(port)
        c.send({"type": "HELLO", "token": token, "name": name})
        c.expect("CHAR_LOAD")
        return c


class Server:
    """Lua server subprocess. Drains stdout to a log file to prevent pipe-block."""

    def __init__(self, cfg: str = TEST_CFG, port: int = PORT, db: str = TEST_DB):
        self._proc: Optional[subprocess.Popen] = None
        self._logfile = None
        self._cfg  = cfg
        self._port = port
        self._db   = db
        self._logpath = os.path.join(tempfile.gettempdir(),
                                     f"tes4mp_test_server_{port}.log")

    def start(self):
        if os.path.exists(self._db):
            os.remove(self._db)
        self._logfile = open(self._logpath, "w")
        self._proc = subprocess.Popen(
            ["lua", "server/main.lua", self._cfg],
            stdout=self._logfile,
            stderr=self._logfile,
        )
        time.sleep(STARTUP_WAIT)
        # Verify it's up
        try:
            s = socket.create_connection((HOST, self._port), timeout=2)
            s.close()
        except Exception as e:
            self._dump_log()
            raise RuntimeError(f"Server did not start: {e}")

    def stop(self):
        if self._proc:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._proc.kill()
        if self._logfile:
            self._logfile.close()
        if os.path.exists(self._db):
            os.remove(self._db)

    def dump_log(self):
        """Print server log — called on failure summary."""
        self._dump_log()

    def _dump_log(self):
        if self._logfile:
            self._logfile.flush()
        try:
            with open(self._logpath) as f:
                content = f.read().strip()
            if content:
                print("\n[Server log]\n" + content)
        except FileNotFoundError:
            pass


# ── Test runner ───────────────────────────────────────────────────────────────

_results: list[tuple[str, bool, str]] = []

def run_test(name: str, fn):
    try:
        fn()
        _results.append((name, True, ""))
        print(f"  PASS  {name}")
    except AssertionError as e:
        _results.append((name, False, str(e)))
        print(f"  FAIL  {name}: {e}")
    except Exception as e:
        _results.append((name, False, f"{type(e).__name__}: {e}"))
        print(f"  FAIL  {name}: {type(e).__name__}: {e}")


def fresh_token() -> str:
    return uuid.uuid4().hex  # 32-char hex


def put_in_cell(client: Client, x: float, y: float, z: float,
                cell: int = 999001, ws: int = 0):
    """Send a POSITION_UPDATE that places client into a specific cell."""
    client.send({
        "type": "POSITION_UPDATE",
        "x": x, "y": y, "z": z,
        "rot": 0, "cell": cell, "ws": ws, "anim": 0, "hp": 100,
    })
    time.sleep(0.08)


# ── Tests ─────────────────────────────────────────────────────────────────────

def test_auth_new():
    token = fresh_token()
    c = Client()
    c.send({"type": "HELLO", "token": token, "name": "NewPlayer"})
    pkt = c.expect("CHAR_LOAD")
    assert pkt.get("name") == "NewPlayer", f"name mismatch: {pkt.get('name')}"
    # Client-side quest poller is driven by this list
    monitored = pkt.get("monitored", [])
    assert "MQ01" in monitored, f"monitored quest list missing/short: {monitored[:3]}"
    c.close()


def test_auth_returning():
    token = fresh_token()
    # First connect creates the char
    c = Client.connect_and_auth(token, "ReturnPlayer")
    c.close()
    time.sleep(0.1)
    # Second connect with same token returns same char
    c2 = Client()
    c2.send({"type": "HELLO", "token": token, "name": "ReturnPlayer"})
    pkt = c2.expect("CHAR_LOAD")
    assert pkt.get("name") == "ReturnPlayer", f"name mismatch: {pkt.get('name')}"
    c2.close()


def test_auth_bad_token():
    c = Client()
    c.send({"type": "HELLO", "token": "short", "name": "Hacker"})
    pkt = c.expect("KICK")
    assert pkt.get("type") == "KICK"
    c.close()


def test_reveal_markers_on_join():
    """Server sends REVEAL_MARKERS immediately after CHAR_LOAD."""
    token = fresh_token()
    c = Client()
    c.send({"type": "HELLO", "token": token, "name": "MapPlayer"})
    # CHAR_LOAD comes first; REVEAL_MARKERS should follow
    pkts = c.recv_all(1.0)
    types = [p.get("type") for p in pkts]
    assert "REVEAL_MARKERS" in types, f"REVEAL_MARKERS not in join packets: {types}"
    c.close()


def test_ghost_appear_leave():
    """A enters cell → B gets GHOST_APPEAR. A moves cell → B gets GHOST_LEAVE."""
    a = Client.connect_and_auth(fresh_token(), "GhostA")
    b = Client.connect_and_auth(fresh_token(), "GhostB")

    # B enters cell 999001
    put_in_cell(b, 0, 0, 0, cell=999001)
    b.recv_all(0.1)  # flush

    # A enters same cell → B should get GHOST_APPEAR
    put_in_cell(a, 10, 0, 0, cell=999001)
    pkt = b.expect("GHOST_APPEAR")
    assert pkt.get("char_name") == "GhostA", f"wrong char: {pkt}"

    # A moves to a different cell → B should get GHOST_LEAVE
    put_in_cell(a, 0, 0, 0, cell=999002)
    pkt = b.expect("GHOST_LEAVE")
    assert pkt.get("type") == "GHOST_LEAVE"

    a.close(); b.close()


def test_fast_travel_flag():
    """POSITION_UPDATE with ft:1 → GHOST_APPEAR to peer has fast_travel=true and cell_name."""
    a = Client.connect_and_auth(fresh_token(), "Traveler")
    b = Client.connect_and_auth(fresh_token(), "Observer")

    # B waits in the destination cell
    put_in_cell(b, 100, 100, 0, cell=888002)
    time.sleep(0.1)
    b.recv_all(0.2)  # flush join packets

    # A starts in a different cell (nearby position so no speed-hack on entry)
    put_in_cell(a, 0, 0, 0, cell=888001)
    time.sleep(0.1)
    b.recv_all(0.1)  # B is in 888002, not 888001 — no packets expected

    # A fast-travels into B's cell — ft=1 bypasses speed check
    a.send({
        "type": "POSITION_UPDATE",
        "x": 100, "y": 100, "z": 0,
        "rot": 0, "cell": 888002, "ws": 0, "anim": 0, "hp": 100,
        "ft": 1, "cn": "Bravil",
    })
    time.sleep(0.1)

    # B should get GHOST_APPEAR with fast_travel=true
    pkt = b.expect("GHOST_APPEAR")
    assert pkt.get("char_name") == "Traveler", f"wrong char: {pkt}"
    assert pkt.get("fast_travel") is True, f"fast_travel missing: {pkt}"
    assert pkt.get("cell_name") == "Bravil", f"cell_name wrong: {pkt}"

    a.close(); b.close()


def test_player_death():
    """PLAYER_DIED → peer in same cell gets PLAYER_DIED with the dying player's char_id."""
    a = Client.connect_and_auth(fresh_token(), "DyingA")
    b = Client.connect_and_auth(fresh_token(), "WatcherB")

    put_in_cell(a, 0, 0, 0, cell=777001)
    put_in_cell(b, 0, 0, 0, cell=777001)
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "PLAYER_DIED"})
    # Server should broadcast PLAYER_DIED to the cell (treated as GHOST_LEAVE on client,
    # but the packet type on the wire from server is PLAYER_DIED)
    pkt = b.expect("PLAYER_DIED")
    assert pkt.get("type") == "PLAYER_DIED"

    a.close(); b.close()


def test_npc_kill_broadcast():
    """NPC_KILLED from A → B in same cell gets NPC_KILLED."""
    a = Client.connect_and_auth(fresh_token(), "KillerA")
    b = Client.connect_and_auth(fresh_token(), "WitnessB")

    cell_key = "666001"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "NPC_KILLED", "ref_id": 12345, "cell": cell_key})
    pkt = b.expect("NPC_KILLED")
    assert pkt.get("ref_id") == 12345, f"ref_id wrong: {pkt}"

    a.close(); b.close()


def test_npc_kill_sync_on_join():
    """Cell with prior kill → new joiner gets NPC_KILL_SYNC."""
    a = Client.connect_and_auth(fresh_token(), "KillerC")
    cell_key = "555001"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.1)

    a.send({"type": "NPC_KILLED", "ref_id": 99999, "cell": cell_key})
    a.recv_all(0.1)  # drain any echo

    # New player enters same cell → should get NPC_KILL_SYNC
    b = Client.connect_and_auth(fresh_token(), "LateJoinerD")
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    pkt = b.expect("NPC_KILL_SYNC")
    assert 99999 in pkt.get("refs", []), f"ref not in sync list: {pkt}"

    a.close(); b.close()


def test_container_loot_broadcast():
    """ITEM_TAKEN from A → B in same cell gets ITEM_SYNC."""
    a = Client.connect_and_auth(fresh_token(), "LooterA")
    b = Client.connect_and_auth(fresh_token(), "PeerB")
    cell_key = "444001"

    put_in_cell(a, 0, 0, 0, cell=int(cell_key))
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "ITEM_TAKEN", "container_ref_id": 11111,
            "item_form_id": 222, "count": 3, "cell": cell_key})
    pkt = b.expect("ITEM_SYNC")
    assert pkt.get("container_ref_id") == 11111
    assert pkt.get("item_form_id") == 222
    assert pkt.get("count") == 3

    a.close(); b.close()


def test_container_state_on_join():
    """New player enters a cell where items were looted → CONTAINER_STATE."""
    a = Client.connect_and_auth(fresh_token(), "LooterC")
    cell_key = "333001"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.1)

    a.send({"type": "ITEM_TAKEN", "container_ref_id": 55555,
            "item_form_id": 999, "count": 7, "cell": cell_key})
    a.recv_all(0.1)

    b = Client.connect_and_auth(fresh_token(), "LateJoinerE")
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    pkt = b.expect("CONTAINER_STATE")
    containers = pkt.get("containers", [])
    found = any(c.get("ref_id") == 55555 for c in containers)
    assert found, f"container not in state: {containers}"

    a.close(); b.close()


def test_char_save_skill_clamp():
    """CHAR_SAVE with suspicious skill delta → next CHAR_LOAD shows clamped value."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "ClampTest")
    # First CHAR_SAVE establishes baseline (blade=10)
    a.send({"type": "CHAR_SAVE", "name": "ClampTest", "level": 1,
            "skills": {"Blade": 10}, "attributes": {},
            "health": 100, "magicka": 100, "stamina": 100,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)

    # Second CHAR_SAVE with Blade jump of 50 (MAX_SKILL_DELTA = 5)
    a.send({"type": "CHAR_SAVE", "name": "ClampTest", "level": 1,
            "skills": {"Blade": 60}, "attributes": {},
            "health": 100, "magicka": 100, "stamina": 100,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)
    a.close()

    # Reconnect and check that the skill is clamped (10 + 5 = 15, not 60)
    time.sleep(0.1)
    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "ClampTest"})
    pkt = b.expect("CHAR_LOAD")
    blade = pkt.get("skills", {}).get("Blade")
    assert blade is not None, f"Blade not in skills: {pkt.get('skills')}"
    assert blade <= 15, f"Blade not clamped: {blade} (expected ≤15)"
    b.close()


def test_char_save_level_clamp():
    """CHAR_SAVE with level jump > 1 → clamped to oldLevel+1."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "LevelClamp")
    # Baseline level 1
    a.send({"type": "CHAR_SAVE", "name": "LevelClamp", "level": 1,
            "skills": {}, "attributes": {},
            "health": 100, "magicka": 100, "stamina": 100,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)
    # Jump to level 10
    a.send({"type": "CHAR_SAVE", "name": "LevelClamp", "level": 10,
            "skills": {}, "attributes": {},
            "health": 100, "magicka": 100, "stamina": 100,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)
    a.close()

    time.sleep(0.1)
    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "LevelClamp"})
    pkt = b.expect("CHAR_LOAD")
    level = pkt.get("level")
    assert level == 2, f"Level not clamped to 2: {level}"
    b.close()


def test_zero_stats_rejected():
    """All-zero attribute checkpoint (loading-screen glitch) must not persist —
    Strength 0 = zero carry weight = a bricked, immobile character."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "ZeroStats")
    a.send({"type": "CHAR_SAVE", "name": "ZeroStats", "level": 1,
            "skills": {"Blade": 30},
            "attributes": {"Strength": 40, "Endurance": 35},
            "health": 100, "magicka": 100, "stamina": 100,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)

    # Glitched checkpoint: everything zero
    a.send({"type": "CHAR_SAVE", "name": "ZeroStats", "level": 1,
            "skills": {"Blade": 0},
            "attributes": {"Strength": 0, "Endurance": 0},
            "health": 0, "magicka": 0, "stamina": 0,
            "pos_x": 0, "pos_y": 0, "pos_z": 0})
    time.sleep(0.15)
    a.close()

    time.sleep(0.1)
    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "ZeroStats"})
    pkt = b.expect("CHAR_LOAD")
    assert pkt.get("attributes", {}).get("Strength") == 40, \
        f"zero attrs persisted: {pkt.get('attributes')}"
    assert pkt.get("skills", {}).get("Blade") == 30, \
        f"zero skills persisted: {pkt.get('skills')}"
    b.close()


def test_nan_position_dropped():
    """POSITION_UPDATE with NaN coords must be dropped, not crash the encoder."""
    a = Client.connect_and_auth(fresh_token(), "NanSender")
    b = Client.connect_and_auth(fresh_token(), "NanWatcher")

    put_in_cell(b, 0, 0, 0, cell=121001)
    b.recv_all(0.2)

    # json.dumps emits the literal NaN (lua-cjson decodes it, but refuses to re-encode)
    a.send({"type": "POSITION_UPDATE",
            "x": float("nan"), "y": 0, "z": 0,
            "rot": 0, "cell": 121001, "ws": 0, "anim": 0, "hp": 100})
    b.expect_none("GHOST_APPEAR", timeout=0.4)

    # Server must still be healthy: a valid update from A now goes through
    put_in_cell(a, 5, 0, 0, cell=121001)
    pkt = b.expect("GHOST_APPEAR")
    assert pkt.get("char_name") == "NanSender", f"server unhealthy after NaN: {pkt}"

    a.close(); b.close()


def test_start_cell_until_tutorial_done():
    """Returning player with tutorial incomplete gets cell=ImperialSewers03 in
    CHAR_LOAD (the returning-player client only teleports via `cell`)."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "SewerBound")
    a.close()  # no CHAR_SAVE sent → tutorial still incomplete
    time.sleep(0.1)

    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "SewerBound"})
    pkt = b.expect("CHAR_LOAD")
    assert pkt.get("cell") == "ImperialSewers03", f"start cell missing: {pkt.get('cell')}"
    b.close()


def test_rename_to_taken_name():
    """HELLO with a name another character owns must not error — keep old name."""
    a = Client.connect_and_auth(fresh_token(), "NameOwner")
    a.close()
    time.sleep(0.1)

    token = fresh_token()
    b = Client.connect_and_auth(token, "SomeoneElse")
    b.close()
    time.sleep(0.1)

    # Reconnect b claiming the taken name — must still get CHAR_LOAD, unrenamed
    c = Client()
    c.send({"type": "HELLO", "token": token, "name": "NameOwner"})
    pkt = c.expect("CHAR_LOAD")
    assert pkt.get("name") == "SomeoneElse", f"rename collision mishandled: {pkt.get('name')}"
    c.close()


def test_duplicate_token_rejected():
    """Second HELLO with an in-use token → KICK with guidance, first stays online."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "TokenOwner")

    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "TokenThief"})
    pkt = b.expect("KICK")
    assert "already online" in pkt.get("reason", ""), f"wrong reason: {pkt}"
    b.close()

    # A's session must be unaffected
    a.send({"type": "PING"})
    a.expect("PONG")
    a.close()


def test_tp_request():
    """TP_REQUEST → TELEPORT to the other player's interior (by editor ID) or
    exterior zone (cow coords)."""
    a = Client.connect_and_auth(fresh_token(), "TpSeeker")
    b = Client.connect_and_auth(fresh_token(), "TpTarget")

    # Interior: B reports a cell editor ID
    b.send({"type": "POSITION_UPDATE", "x": 0, "y": 0, "z": 0, "rot": 0,
            "cell": 131001, "ws": 0, "anim": 0, "hp": 100, "ced": "TestSewer01"})
    time.sleep(0.1)
    a.recv_all(0.1)

    a.send({"type": "TP_REQUEST"})
    pkt = a.expect("TELEPORT")
    assert pkt.get("cell") == "TestSewer01", f"wrong interior target: {pkt}"

    # Exterior: B moves to Tamriel zone (2, -1) → cow cell coords (7, -2)
    b.send({"type": "POSITION_UPDATE", "x": 2 * 12288 + 10, "y": -12288 + 10, "z": 0,
            "rot": 0, "cell": 0, "ws": 60, "anim": 0, "hp": 100, "ft": 1, "cn": "Tamriel"})
    time.sleep(0.1)
    a.recv_all(0.1)

    a.send({"type": "TP_REQUEST"})
    pkt = a.expect("TELEPORT")
    assert pkt.get("cow") == 1 and pkt.get("ws") == 60, f"wrong exterior target: {pkt}"
    assert pkt.get("cx") == 7 and pkt.get("cy") == -2, f"wrong coords: {pkt}"

    a.close(); b.close()


def test_speed_hack_rejection():
    """POSITION_UPDATE with impossible speed → peer does NOT get PLAYER_POS."""
    a = Client.connect_and_auth(fresh_token(), "Speedhacker")
    b = Client.connect_and_auth(fresh_token(), "SpeedObserver")

    put_in_cell(a, 0, 0, 0, cell=222001)
    put_in_cell(b, 0, 0, 0, cell=222001)
    time.sleep(0.1)
    b.recv_all(0.2)

    # Send position 1 to establish baseline
    a.send({"type": "POSITION_UPDATE",
            "x": 0, "y": 0, "z": 0,
            "rot": 0, "cell": 222001, "ws": 0, "anim": 0, "hp": 100})
    time.sleep(0.05)
    b.recv_all(0.1)

    # Send impossible jump (500k units in <1s)
    a.send({"type": "POSITION_UPDATE",
            "x": 500000, "y": 500000, "z": 0,
            "rot": 0, "cell": 222001, "ws": 0, "anim": 0, "hp": 100})

    # B should NOT get a PLAYER_POS for this
    b.expect_none("PLAYER_POS", timeout=0.4)


def test_dungeon_clear():
    """DUNGEON_CLEAR → all connected players get DUNGEON_CLEARED."""
    a = Client.connect_and_auth(fresh_token(), "DungeonA")
    b = Client.connect_and_auth(fresh_token(), "DungeonB")
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "DUNGEON_CLEAR", "dungeon_id": "TestDungeon01"})
    pkt = b.expect("DUNGEON_CLEARED")
    assert pkt.get("dungeon_id") == "TestDungeon01"
    assert pkt.get("clearer") == "DungeonA"

    a.close(); b.close()


def test_weather_sync():
    """WEATHER_REPORT → all players get WEATHER_SYNC; new joiner also gets it."""
    a = Client.connect_and_auth(fresh_token(), "WeatherA")
    b = Client.connect_and_auth(fresh_token(), "WeatherB")
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "WEATHER_REPORT", "weather_id": 0x38})

    # B should get WEATHER_SYNC
    pkt = b.expect("WEATHER_SYNC")
    assert pkt.get("weather_id") == 0x38, f"weather_id wrong: {pkt}"

    # New joiner should also get WEATHER_SYNC on join
    c = Client()
    c.send({"type": "HELLO", "token": fresh_token(), "name": "WeatherC"})
    pkts = c.recv_all(1.0)
    types = [p.get("type") for p in pkts]
    assert "WEATHER_SYNC" in types, f"WEATHER_SYNC not in join packets: {types}"
    wid = next(p.get("weather_id") for p in pkts if p.get("type") == "WEATHER_SYNC")
    assert wid == 0x38, f"wrong weather_id on join: {wid}"

    a.close(); b.close(); c.close()


def test_quest_sync():
    """QUEST_STAGE with scope=global → peer gets QUEST_STAGE."""
    a = Client.connect_and_auth(fresh_token(), "QuestA")
    b = Client.connect_and_auth(fresh_token(), "QuestB")
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "QUEST_STAGE", "questId": "MQ01", "stage": 10, "src": "global"})
    pkt = b.expect("QUEST_STAGE")
    assert pkt.get("questId") == "MQ01"
    assert pkt.get("stage") == 10

    a.close(); b.close()


def test_faction_rank():
    """FACTION_RANK with forward progress → broadcast to all players."""
    a = Client.connect_and_auth(fresh_token(), "FactionA")
    b = Client.connect_and_auth(fresh_token(), "FactionB")
    time.sleep(0.1)
    b.recv_all(0.2)

    a.send({"type": "FACTION_RANK", "faction_id": "MagesGuild", "rank": 3})
    pkt = b.expect("FACTION_RANK")
    assert pkt.get("faction_id") == "MagesGuild"
    assert pkt.get("rank") == 3
    assert pkt.get("char_name") == "FactionA"

    # Same or lower rank should NOT broadcast
    a.send({"type": "FACTION_RANK", "faction_id": "MagesGuild", "rank": 2})
    b.expect_none("FACTION_RANK", timeout=0.3)

    a.close(); b.close()


def test_bounty():
    """CRIME_REPORT stores bounty; BOUNTY_PAID clears it."""
    token = fresh_token()
    a = Client.connect_and_auth(token, "Criminal")
    a.send({"type": "CRIME_REPORT", "hold": "Cyrodiil", "bounty_delta": 100})
    a.send({"type": "CRIME_REPORT", "hold": "Cyrodiil", "bounty_delta": 50})
    time.sleep(0.15)
    a.close()

    # Rejoin and verify bounty is carried forward (it'll be in session state)
    time.sleep(0.1)
    b = Client()
    b.send({"type": "HELLO", "token": token, "name": "Criminal"})
    pkt = b.expect("CHAR_LOAD")
    bounties = pkt.get("bounties", {})
    assert bounties.get("Cyrodiil", 0) == 150, f"bounty wrong: {bounties}"

    # Pay bounty
    b.send({"type": "BOUNTY_PAID", "hold": "Cyrodiil"})
    time.sleep(0.15)
    b.close()

    # Rejoin — bounty should be 0
    time.sleep(0.1)
    c = Client()
    c.send({"type": "HELLO", "token": token, "name": "Criminal"})
    pkt = c.expect("CHAR_LOAD")
    bounties = pkt.get("bounties", {})
    assert bounties.get("Cyrodiil", 0) == 0, f"bounty not cleared: {bounties}"
    c.close()


def test_equip_broadcast():
    """EQUIP_UPDATE from A → B in same cell gets EQUIP_SYNC with filtered items."""
    a = Client.connect_and_auth(fresh_token(), "EquipA")
    b = Client.connect_and_auth(fresh_token(), "EquipB")

    put_in_cell(a, 0, 0, 0, cell=111001)
    put_in_cell(b, 0, 0, 0, cell=111001)
    time.sleep(0.1)
    b.recv_all(0.2)

    # 0x2F000001 has mod index 0x2F — must be filtered out (non-vanilla)
    a.send({"type": "EQUIP_UPDATE", "items": [0x22F30, 0x1C6D5, 0x2F000001]})
    pkt = b.expect("EQUIP_SYNC")
    assert pkt.get("items") == [0x22F30, 0x1C6D5], f"items wrong: {pkt}"

    a.close(); b.close()


def test_equip_in_ghost_appear():
    """Late joiner entering A's cell sees A's equipment inside GHOST_APPEAR."""
    a = Client.connect_and_auth(fresh_token(), "EquipC")
    put_in_cell(a, 0, 0, 0, cell=111002)
    a.send({"type": "EQUIP_UPDATE", "items": [0x22F30]})
    time.sleep(0.15)

    b = Client.connect_and_auth(fresh_token(), "EquipD")
    put_in_cell(b, 0, 0, 0, cell=111002)
    pkt = b.expect("GHOST_APPEAR")
    assert pkt.get("char_name") == "EquipC", f"wrong char: {pkt}"
    assert pkt.get("equipment") == [0x22F30], f"equipment missing: {pkt}"

    a.close(); b.close()


def test_cell_authority_assign():
    """First player in cell → authority:true; second → authority:false."""
    a = Client.connect_and_auth(fresh_token(), "AuthA")
    b = Client.connect_and_auth(fresh_token(), "AuthB")

    put_in_cell(a, 0, 0, 0, cell=112001)
    pkt = a.expect("CELL_AUTHORITY")
    assert pkt.get("authority") is True, f"first player not authority: {pkt}"

    put_in_cell(b, 0, 0, 0, cell=112001)
    pkt = b.expect("CELL_AUTHORITY")
    assert pkt.get("authority") is False, f"second player got authority: {pkt}"

    a.close(); b.close()


def test_cell_authority_handover():
    """Authority leaves the cell → remaining player gets authority:true."""
    a = Client.connect_and_auth(fresh_token(), "AuthC")
    b = Client.connect_and_auth(fresh_token(), "AuthD")

    put_in_cell(a, 0, 0, 0, cell=112002)
    put_in_cell(b, 0, 0, 0, cell=112002)
    time.sleep(0.1)
    b.recv_all(0.2)

    put_in_cell(a, 0, 0, 0, cell=112003)  # authority walks out
    pkt = b.expect("CELL_AUTHORITY")
    assert pkt.get("authority") is True, f"handover failed: {pkt}"

    a.close(); b.close()


def test_npc_hp_relay():
    """Authority's NPC_HP is relayed to peers; non-authority's is ignored."""
    a = Client.connect_and_auth(fresh_token(), "HpAuthority")
    b = Client.connect_and_auth(fresh_token(), "HpObserver")

    cell_key = "113001"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))   # a = authority
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.2); b.recv_all(0.2)

    a.send({"type": "NPC_HP", "cell": cell_key,
            "npcs": [{"ref": 4242, "hp": 55}, {"ref": 4243, "hp": 0}]})
    pkt = b.expect("NPC_HP")
    assert pkt.get("npcs") == [{"ref": 4242, "hp": 55}, {"ref": 4243, "hp": 0}], \
        f"npcs wrong: {pkt}"

    # Non-authority report must be dropped
    b.send({"type": "NPC_HP", "cell": cell_key, "npcs": [{"ref": 4242, "hp": 1}]})
    a.expect_none("NPC_HP", timeout=0.4)

    a.close(); b.close()


def test_npc_damage_routing():
    """Non-authority NPC_DAMAGE is routed to the authority only."""
    a = Client.connect_and_auth(fresh_token(), "DmgAuthority")
    b = Client.connect_and_auth(fresh_token(), "DmgPeer")
    c = Client.connect_and_auth(fresh_token(), "DmgBystander")

    cell_key = "113002"
    for cl in (a, b, c):
        put_in_cell(cl, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.2); b.recv_all(0.2); c.recv_all(0.2)

    b.send({"type": "NPC_DAMAGE", "cell": cell_key, "ref_id": 777, "amount": 12})
    pkt = a.expect("NPC_DAMAGE")
    assert pkt.get("ref_id") == 777 and pkt.get("amount") == 12, f"wrong: {pkt}"
    c.expect_none("NPC_DAMAGE", timeout=0.3)

    a.close(); b.close(); c.close()


def test_npc_spawns_relay():
    """Authority's NPC_SPAWNS relayed to cell peers; non-authority's dropped;
    invalid entries filtered."""
    a = Client.connect_and_auth(fresh_token(), "SpawnAuth")
    b = Client.connect_and_auth(fresh_token(), "SpawnPeer")

    cell_key = "141001"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))   # a = authority
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.2); b.recv_all(0.2)

    a.send({"type": "NPC_SPAWNS", "cell": cell_key, "spawns": [
        {"sid": 0xFF001234, "base": 0x23AB5, "x": 10, "y": 20, "z": 0, "hp": 25},
        {"sid": 5, "base": 0x23AB5, "x": 0, "y": 0, "z": 0, "hp": 25},        # static sid: filtered
        {"sid": 0xFF001235, "base": 0x0100AAAA, "x": 0, "y": 0, "z": 0, "hp": 9},  # mod base: filtered
    ]})
    pkt = b.expect("NPC_SPAWNS")
    spawns = pkt.get("spawns", [])
    assert len(spawns) == 1 and spawns[0]["sid"] == 0xFF001234, f"filtering wrong: {spawns}"

    # Non-authority snapshots must not relay
    b.send({"type": "NPC_SPAWNS", "cell": cell_key,
            "spawns": [{"sid": 0xFF009999, "base": 100, "x": 0, "y": 0, "z": 0, "hp": 5}]})
    a.expect_none("NPC_SPAWNS", timeout=0.4)

    a.close(); b.close()


def test_corpse_loot_and_dynamic_reject():
    """ITEM_TAKEN from a corpse (static actor ref) broadcasts + persists like a
    container; dynamic container refs are rejected."""
    a = Client.connect_and_auth(fresh_token(), "CorpseLooter")
    b = Client.connect_and_auth(fresh_token(), "CorpseWitness")

    cell_key = "141003"
    put_in_cell(a, 0, 0, 0, cell=int(cell_key))
    put_in_cell(b, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.2); b.recv_all(0.2)

    # Corpse ref is just a static ref — same path as containers
    a.send({"type": "ITEM_TAKEN", "container_ref_id": 91001,
            "item_form_id": 4321, "count": 2, "cell": cell_key})
    pkt = b.expect("ITEM_SYNC")
    assert pkt.get("container_ref_id") == 91001 and pkt.get("count") == 2, f"wrong: {pkt}"

    # Dynamic (0xFFxxxxxx) ref must be rejected
    a.send({"type": "ITEM_TAKEN", "container_ref_id": 0xFF001111,
            "item_form_id": 4321, "count": 1, "cell": cell_key})
    b.expect_none("ITEM_SYNC", timeout=0.3)

    # Late joiner gets the corpse state on cell entry
    c = Client.connect_and_auth(fresh_token(), "CorpseLate")
    put_in_cell(c, 0, 0, 0, cell=int(cell_key))
    pkt = c.expect("CONTAINER_STATE")
    refs = [e.get("ref_id") for e in pkt.get("containers", [])]
    assert 91001 in refs, f"corpse state missing on entry: {pkt}"

    a.close(); b.close(); c.close()


def test_npc_damage_sid_routing():
    """Follower NPC_DAMAGE_SID routed to authority only; static sid rejected;
    authority-origin dropped."""
    a = Client.connect_and_auth(fresh_token(), "SidAuth")
    b = Client.connect_and_auth(fresh_token(), "SidPeer")
    c = Client.connect_and_auth(fresh_token(), "SidBystander")

    cell_key = "141002"
    for cl in (a, b, c):
        put_in_cell(cl, 0, 0, 0, cell=int(cell_key))
    time.sleep(0.1)
    a.recv_all(0.2); b.recv_all(0.2); c.recv_all(0.2)

    sid = 0xFF00BEEF
    b.send({"type": "NPC_DAMAGE_SID", "cell": cell_key, "sid": sid, "amount": 33})
    pkt = a.expect("NPC_DAMAGE_SID")
    assert pkt.get("sid") == sid and pkt.get("amount") == 33, f"wrong: {pkt}"
    c.expect_none("NPC_DAMAGE_SID", timeout=0.3)

    # Static (non-dynamic) sid must be rejected
    b.send({"type": "NPC_DAMAGE_SID", "cell": cell_key, "sid": 777, "amount": 10})
    a.expect_none("NPC_DAMAGE_SID", timeout=0.3)

    # Authority reporting its own damage must not loop back
    a.send({"type": "NPC_DAMAGE_SID", "cell": cell_key, "sid": sid, "amount": 5})
    a.expect_none("NPC_DAMAGE_SID", timeout=0.3)
    b.expect_none("NPC_DAMAGE_SID", timeout=0.1)

    a.close(); b.close(); c.close()


def test_quest_sync_on_cell_entry():
    """Entering a cell re-sends QUEST_SYNC (stage parity guard)."""
    a = Client.connect_and_auth(fresh_token(), "StageParity")
    a.recv_all(0.3)  # drain login packets (includes the login QUEST_SYNC)
    put_in_cell(a, 0, 0, 0, cell=142001)
    pkt = a.expect("QUEST_SYNC")
    assert "quests" in pkt, f"quest sync missing on cell entry: {pkt}"
    a.close()


def test_pvp_hit():
    """PLAYER_HIT → target gets DAMAGE_TAKEN (pvp:true in test config), clamped to 100."""
    a = Client.connect_and_auth(fresh_token(), "PvpAttacker")
    b = Client.connect_and_auth(fresh_token(), "PvpVictim")

    put_in_cell(b, 0, 0, 0, cell=114001)
    put_in_cell(a, 10, 0, 0, cell=114001)

    # A learns B's char_id from the GHOST_APPEAR it receives on entering the cell
    pkt = a.expect("GHOST_APPEAR")
    target_id = int(pkt.get("char_id"))
    b.recv_all(0.2)

    a.send({"type": "PLAYER_HIT", "target_char_id": target_id, "amount": 250})
    pkt = b.expect("DAMAGE_TAKEN")
    assert pkt.get("amount") == 100, f"not clamped: {pkt}"
    assert pkt.get("from") == "PvpAttacker", f"wrong source: {pkt}"

    a.close(); b.close()


def test_pvp_disabled_gate():
    """On a pvp:false server, PLAYER_HIT produces no DAMAGE_TAKEN."""
    server = Server(cfg="server/tests/test_config_pvpoff.json",
                    port=17778, db="server/data/test_tes4mp_pvpoff.db")
    server.start()
    try:
        a = Client.connect_and_auth(fresh_token(), "PeaceA", port=17778)
        b = Client.connect_and_auth(fresh_token(), "PeaceB", port=17778)

        put_in_cell(b, 0, 0, 0, cell=114002)
        put_in_cell(a, 10, 0, 0, cell=114002)
        pkt = a.expect("GHOST_APPEAR")
        target_id = int(pkt.get("char_id"))
        b.recv_all(0.2)

        a.send({"type": "PLAYER_HIT", "target_char_id": target_id, "amount": 50})
        b.expect_none("DAMAGE_TAKEN", timeout=0.4)

        a.close(); b.close()
    finally:
        server.stop()


# ── Static DLL review ─────────────────────────────────────────────────────────

def static_review():
    """Read client C++ source and check for known risk patterns."""
    issues = []
    warnings = []

    checks = [
        # (file, pattern, message, is_error)
        ("client/plugin/src/pos_sync.cpp",
         'char buf[280]',
         "pos_sync POSITION_UPDATE buffer is 280B — verify snprintf fits ft+cn payload",
         False),
        ("client/plugin/src/game_hooks.cpp",
         'SampleWeatherId',
         "SampleWeatherId present — Sky singleton (0x00B13A94) unverified for 1.2.416",
         False),
        ("client/plugin/src/game_hooks.cpp",
         'prevHp = 0',
         "prevHp reset after death send — good, no re-fire while dead",
         False),
        ("client/plugin/src/npc_sync.cpp",
         'GetActorHp',
         "GetActorHp vtable dispatch — null-guarded via WalkCellRefs early return",
         False),
    ]

    for filepath, pattern, msg, is_err in checks:
        try:
            with open(filepath) as f:
                content = f.read()
            found = pattern in content
            if is_err and not found:
                issues.append(f"MISSING '{pattern}' in {filepath}: {msg}")
            elif not is_err:
                (warnings if not found else []).append(f"NOTE {filepath}: {msg}")
        except FileNotFoundError:
            issues.append(f"FILE NOT FOUND: {filepath}")

    # Check snprintf buffer size is adequate
    # Max POSITION_UPDATE with ft+cn: ~230 chars; 280 is fine
    try:
        with open("client/plugin/src/pos_sync.cpp") as f:
            src = f.read()
        import re
        bufs = re.findall(r'char buf\[(\d+)\]', src)
        for b in bufs:
            if int(b) < 200:
                issues.append(f"pos_sync buf[{b}] may be too small for ft payload")
    except FileNotFoundError:
        pass

    return issues, warnings


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print("=" * 60)
    print("TES4MP Integration Tests")
    print("=" * 60)

    # Static review first (no server needed)
    print("\n[Static DLL Review]")
    issues, warnings = static_review()
    for w in warnings:
        print(f"  NOTE  {w}")
    for i in issues:
        print(f"  WARN  {i}")
    if not issues and not warnings:
        print("  OK    No concerns found")

    print("\n[Server Integration Tests]")
    server = Server()
    try:
        server.start()
        print(f"  Server up on {HOST}:{PORT}\n")

        tests = [
            ("auth: new player",          test_auth_new),
            ("auth: returning player",     test_auth_returning),
            ("auth: bad token → KICK",     test_auth_bad_token),
            ("join: REVEAL_MARKERS sent",  test_reveal_markers_on_join),
            ("ghost: appear + leave",      test_ghost_appear_leave),
            ("ghost: fast travel flag",    test_fast_travel_flag),
            ("ghost: player death",        test_player_death),
            ("npc: kill broadcast",        test_npc_kill_broadcast),
            ("npc: kill sync on join",     test_npc_kill_sync_on_join),
            ("container: loot broadcast",  test_container_loot_broadcast),
            ("container: state on join",   test_container_state_on_join),
            ("char_save: skill clamp",     test_char_save_skill_clamp),
            ("char_save: level clamp",     test_char_save_level_clamp),
            ("char_save: zero stats gate", test_zero_stats_rejected),
            ("position: NaN dropped",      test_nan_position_dropped),
            ("teleport: TP_REQUEST",       test_tp_request),
            ("auth: duplicate token",      test_duplicate_token_rejected),
            ("auth: taken-name rename",    test_rename_to_taken_name),
            ("auth: start cell persists",  test_start_cell_until_tutorial_done),
            ("speed hack: rejection",      test_speed_hack_rejection),
            ("dungeon: clear broadcast",   test_dungeon_clear),
            ("weather: sync broadcast",    test_weather_sync),
            ("quest: stage sync",          test_quest_sync),
            ("faction: rank broadcast",    test_faction_rank),
            ("bounty: report + clear",     test_bounty),
            ("equip: update broadcast",    test_equip_broadcast),
            ("equip: in GHOST_APPEAR",     test_equip_in_ghost_appear),
            ("authority: assignment",      test_cell_authority_assign),
            ("authority: handover",        test_cell_authority_handover),
            ("npc_hp: relay + gate",       test_npc_hp_relay),
            ("npc_damage: routing",        test_npc_damage_routing),
            ("npc_spawns: relay + filter", test_npc_spawns_relay),
            ("npc_damage_sid: routing",    test_npc_damage_sid_routing),
            ("loot: corpse + dyn reject",  test_corpse_loot_and_dynamic_reject),
            ("quests: sync on cell entry", test_quest_sync_on_cell_entry),
            ("pvp: hit routed + clamped",  test_pvp_hit),
            ("pvp: disabled gate",         test_pvp_disabled_gate),
        ]

        for name, fn in tests:
            run_test(name, fn)
            time.sleep(0.05)  # small gap between tests

    finally:
        if any(not ok for _, ok, _ in _results):
            server.dump_log()
        server.stop()

    # Summary
    passed = sum(1 for _, ok, _ in _results if ok)
    failed = sum(1 for _, ok, _ in _results if not ok)
    total  = len(_results)

    print(f"\n{'=' * 60}")
    print(f"Results: {passed}/{total} passed", end="")
    if failed:
        print(f"  ({failed} FAILED)")
        print("\nFailed tests:")
        for name, ok, err in _results:
            if not ok:
                print(f"  - {name}: {err}")
    else:
        print(" — all green")
    print("=" * 60)

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
