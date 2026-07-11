#!/usr/bin/env bash
# Wipe the TES4MP character database.
#
#   ./wipe_characters.sh          wipe characters (+ their skills/attrs/vitals/
#                                 quests/appearance/equipment/factions/bounties/
#                                 parties/audit log)
#   ./wipe_characters.sh --world  ALSO wipe shared world state (kills, loot,
#                                 world items, dungeon clears, global quests)
#   ./wipe_characters.sh -y ...   skip the confirmation prompt
#
# Player tokens live client-side (%APPDATA%\TES4MP\token.txt) — after a wipe,
# everyone reconnects as a fresh character with the same token.
set -euo pipefail
cd "$(dirname "$0")"

DB="server/data/tes4mp.db"
[[ -f "$DB" ]] || { echo "No database at $DB — nothing to wipe."; exit 0; }

# Refuse to touch the DB under a live server (WAL writes would race).
if pgrep -f "lua.*server/main.lua" >/dev/null 2>&1; then
    echo "Server appears to be running — stop it first." >&2
    exit 1
fi

WIPE_WORLD=0
CONFIRM=1
for arg in "$@"; do
    case "$arg" in
        --world) WIPE_WORLD=1 ;;
        -y)      CONFIRM=0 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

CHAR_COUNT=$(sqlite3 "$DB" "SELECT COUNT(*) FROM characters;")
echo "Database: $DB ($CHAR_COUNT characters)"
[[ $WIPE_WORLD -eq 1 ]] && echo "Also wiping world state (kills, loot, dungeon clears, global quests)."

if [[ $CONFIRM -eq 1 ]]; then
    read -r -p "Wipe? This cannot be undone. [y/N] " reply
    [[ "$reply" == "y" || "$reply" == "Y" ]] || { echo "Aborted."; exit 1; }
fi

# Snapshot first — cheap insurance.
BACKUP="server/data/tes4mp.db.bak.$(date +%Y%m%d-%H%M%S)"
sqlite3 "$DB" ".backup '$BACKUP'"
echo "Backup: $BACKUP"

sqlite3 "$DB" <<'SQL'
BEGIN;
DELETE FROM party_members;
DELETE FROM parties;
DELETE FROM character_skills;
DELETE FROM character_attributes;
DELETE FROM character_vitals;
DELETE FROM character_quests;
DELETE FROM character_appearance;
DELETE FROM character_equipment;
DELETE FROM faction_ranks;
DELETE FROM bounties;
DELETE FROM audit_log;
DELETE FROM characters;
COMMIT;
SQL

if [[ $WIPE_WORLD -eq 1 ]]; then
    sqlite3 "$DB" <<'SQL'
BEGIN;
DELETE FROM killed_refs;
DELETE FROM container_items;
DELETE FROM world_items_taken;
DELETE FROM dungeon_clears;
DELETE FROM global_quests;
COMMIT;
SQL
fi

sqlite3 "$DB" "VACUUM;"
echo "Done. $CHAR_COUNT characters wiped."
