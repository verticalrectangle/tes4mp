#!/usr/bin/env python3
"""
Generate a TES4MP.esp skeleton for Oblivion.

The ESP contains:
  - 3 SCPT records with full script source embedded (SCTX)
    and a minimal compiled stub (SCDA) so the CS can display
    and recompile each script with one click.
  - 2 QUST records (Start Game Enabled, priority 100) linked to their scripts.
  - 1 MISC record (TES4MP Terminal) linked to the menu script.

After generation, open the CS, load TES4MP.esp, then for each script:
  Gameplay → Edit Scripts → open each TES4MP script → click Compile → OK.
Then File → Save. Done.
"""

import struct, os, sys, shutil

BASE   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRDIR = os.path.join(BASE, "client", "esp", "scripts")
OUTDIR = os.path.join(BASE, "client", "esp")
OBDIR  = os.path.expanduser("~/.local/share/Steam/steamapps/common/Oblivion")

OUT_ESP     = os.path.join(OUTDIR, "TES4MP.esp")
INSTALL_ESP = os.path.join(OBDIR, "Data", "TES4MP.esp")

# Assign stable form IDs within this plugin (index 0x00)
FID = {
    "SCPT_MAIN":   0x00000D62,
    "SCPT_QUESTS": 0x00000D63,
    "SCPT_MENU":   0x00000D64,
    "QUST_MAIN":   0x00000D65,
    "QUST_QUESTS": 0x00000D66,
    "MISC_TERM":   0x00000D67,
    # Ghost NPC holding cell + 4 placed NPC refs (initially disabled)
    "CELL_GHOST":  0x00000D68,
    "ACHR_GHOST1": 0x00000D69,
    "ACHR_GHOST2": 0x00000D6A,
    "ACHR_GHOST3": 0x00000D6B,
    "ACHR_GHOST4": 0x00000D6C,
}
NEXT_OBJECT_ID = 0x00000D6D

# Vanilla Imperial Watch guard from Oblivion.esm — used as ghost NPC base.
# High byte 0x00 = first master file (Oblivion.esm).
GHOST_BASE_NPC = 0x0001C34F


# ── Binary helpers ────────────────────────────────────────────────────────────

def sub(t: str, data) -> bytes:
    """Subrecord: type(4) + size(2) + data."""
    if isinstance(data, str):
        data = (data + "\0").encode("latin-1")
    return t.encode("ascii") + struct.pack("<H", len(data)) + data

def rec(t: str, form_id: int, flags: int, *subs) -> bytes:
    """Record: 20-byte header + subrecords."""
    data = b"".join(subs)
    return (
        t.encode("ascii")
        + struct.pack("<IIII", len(data), flags, form_id, 0)
        + data
    )

def grp(label: bytes, records: bytes) -> bytes:
    """Top-level GRUP (type 0): 20-byte header (size includes the header itself)."""
    total = 20 + len(records)
    return b"GRUP" + struct.pack("<I", total) + label + struct.pack("<II", 0, 0) + records

def grp_typed(label: bytes, group_type: int, records: bytes) -> bytes:
    """Typed GRUP: 2=interior block, 3=interior sub-block, 6=cell persistent children."""
    total = 20 + len(records)
    return b"GRUP" + struct.pack("<I", total) + label + struct.pack("<II", group_type, 0) + records


# ── Record builders ───────────────────────────────────────────────────────────

def make_scpt(form_id: int, editor_id: str, script_type: int, source: str) -> bytes:
    """SCPT record.  script_type: 0=object, 1=quest."""
    # Minimal compiled stub: a single Return opcode (0x001D), no params.
    # The CS will replace SCDA when the user clicks Compile.
    scda = b"\x1D\x00\x00\x00"
    schr = struct.pack("<IIIII",
        0,              # unused
        len(scda),      # ScriptDataSize
        0,              # LocalVariableCount
        0,              # ReferenceCount
        script_type,
    )
    return rec("SCPT", form_id, 0,
        sub("EDID", editor_id),
        sub("SCHR", schr),
        sub("SCDA", scda),
        sub("SCTX", source.encode("latin-1", errors="replace") + b"\x00"),
    )

def make_qust(form_id: int, editor_id: str, script_fid: int) -> bytes:
    """QUST record: Start Game Enabled, priority 100."""
    return rec("QUST", form_id, 0,
        sub("EDID", editor_id),
        sub("SCRI", struct.pack("<I", script_fid)),
        sub("DATA", struct.pack("<BB", 0x01, 100)),
    )

def make_cell(form_id: int, editor_id: str) -> bytes:
    """Interior CELL record."""
    return rec("CELL", form_id, 0,
        sub("EDID", editor_id),
        sub("DATA", struct.pack("<B", 0x01)),  # 0x01 = interior flag
    )

def make_achr(form_id: int, editor_id: str, base_npc_fid: int) -> bytes:
    """ACHR: placed NPC reference, initially disabled."""
    return rec("ACHR", form_id, 0x00000800,  # 0x800 = Initially Disabled
        sub("EDID", editor_id),
        sub("NAME", struct.pack("<I", base_npc_fid)),
        sub("DATA", struct.pack("<ffffff", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)),
    )

def make_misc(form_id: int, editor_id: str, display: str, script_fid: int) -> bytes:
    """MISC record: weightless, worthless inventory item."""
    return rec("MISC", form_id, 0,
        sub("EDID", editor_id),
        sub("FULL", display),
        sub("DATA", struct.pack("<if", 0, 0.0)),
        sub("SCRI", struct.pack("<I", script_fid)),
    )


# ── Main ──────────────────────────────────────────────────────────────────────

def read_script(name: str) -> str:
    path = os.path.join(SCRDIR, name)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()

def build_esp() -> bytes:
    src_main   = read_script("TES4MPMain.txt")
    src_quests = read_script("TES4MPQuests.txt")
    src_menu   = read_script("TES4MPMenu.txt")

    # TES4 header
    hedr_data = struct.pack("<fII", 0.94, 10, NEXT_OBJECT_ID)
    tes4 = rec("TES4", 0, 0,
        sub("HEDR", hedr_data),
        sub("CNAM", "TES4MP"),
        sub("SNAM", "TES4MP: quest sync, RBAC, commands."),
        sub("MAST", "Oblivion.esm"),
        sub("DATA", struct.pack("<Q", 0)),
    )

    # Scripts
    scpt_main   = make_scpt(FID["SCPT_MAIN"],   "TES4MPMain",   1, src_main)
    scpt_quests = make_scpt(FID["SCPT_QUESTS"], "TES4MPQuests", 1, src_quests)
    scpt_menu   = make_scpt(FID["SCPT_MENU"],   "TES4MPMenu",   0, src_menu)

    # Quests
    qust_main   = make_qust(FID["QUST_MAIN"],   "TES4MPMainQuest",   FID["SCPT_MAIN"])
    qust_quests = make_qust(FID["QUST_QUESTS"], "TES4MPQuestsQuest", FID["SCPT_QUESTS"])

    # Terminal misc item
    misc_term = make_misc(FID["MISC_TERM"], "TES4MPTerminal", "TES4MP Terminal", FID["SCPT_MENU"])

    # Ghost NPC refs — 4 persistent disabled ACHRs in a private interior cell.
    # The plugin enables/disables them at runtime via prid+enable/disable+moveto.
    achr_records = b"".join(
        make_achr(FID[f"ACHR_GHOST{i}"], f"TES4MPGhostACHR0{i}", GHOST_BASE_NPC)
        for i in range(1, 5)
    )
    ghost_cell = (
        make_cell(FID["CELL_GHOST"], "TES4MPGhostHolding")
        + grp_typed(struct.pack("<I", FID["CELL_GHOST"]), 6, achr_records)
    )
    interior_block = grp_typed(
        struct.pack("<I", 0), 2,
        grp_typed(struct.pack("<I", 0), 3, ghost_cell)
    )

    return (
        tes4
        + grp(b"SCPT", scpt_main + scpt_quests + scpt_menu)
        + grp(b"QUST", qust_main + qust_quests)
        + grp(b"MISC", misc_term)
        + interior_block
    )

if __name__ == "__main__":
    print("Generating TES4MP.esp ...")
    data = build_esp()

    with open(OUT_ESP, "wb") as f:
        f.write(data)
    print(f"  Written: {OUT_ESP}  ({len(data):,} bytes)")

    print(f"  Installing to: {INSTALL_ESP}")
    os.makedirs(os.path.dirname(INSTALL_ESP), exist_ok=True)
    shutil.copy2(OUT_ESP, INSTALL_ESP)
    print("  Done.")
    print()
    print("Next steps:")
    print("  1. Launch the Construction Set (use the TES4MP-CS desktop shortcut)")
    print("  2. File → Data → select TES4MP.esp as Active File → OK")
    print("  3. Gameplay → Edit Scripts")
    print("  4. Open TES4MPMain   → click Compile → OK")
    print("  5. Open TES4MPQuests → click Compile → OK")
    print("  6. Open TES4MPMenu   → click Compile → OK")
    print("  7. File → Save")
    print("  That's it — the .esp is ready.")
