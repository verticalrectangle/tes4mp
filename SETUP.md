# TES4MP Client Setup

Multiplayer for **The Elder Scrolls IV: Oblivion** (Steam, version 1.2.416 — not Remastered).

---

## Requirements

- Oblivion on Steam (version 1.2.416)
- [OBSE 0021](https://obse.silverlock.org/) — the script extender that loads TES4MP

---

## Step 1 — Install OBSE

Download OBSE 0021 and copy these files into your Oblivion folder
(`Steam/steamapps/common/Oblivion/`):

```
obse_1_2_416.dll
obse_editor_1_2.dll
obse_loader.exe
obse_steam_loader.dll
```

---

## Step 2 — Install TES4MP

Copy these two files into `Oblivion/Data/OBSE/Plugins/` (create the folder if it doesn't exist):

```
TES4MP.dll
TES4MP.ini
```

Open `TES4MP.ini` and set the server address:

```ini
[Server]
Host=<server IP or hostname>
Port=7777
```

---

## Step 3 — Patch for Steam (Linux only)

On Linux, OBSE's launcher refuses to run for Steam copies of Oblivion. Two quick binary patches fix this. Run in a terminal:

```bash
cd ~/.local/share/Steam/steamapps/common/Oblivion
printf '\x90\x90\x90' | dd conv=notrunc of=obse_loader.exe bs=1 seek=$((0x14cb))
printf 'obse_loader\x00' | dd conv=notrunc of=OblivionLauncher.exe bs=1 seek=$((0x1347c))
```

These patches are safe and specific to Oblivion 1.2.416. They only modify the launcher binaries, not the game itself.

**Windows users skip this step** — OBSE works out of the box via `obse_steam_loader.dll`.

---

## Step 4 — Configure Steam (Linux only)

In Steam → right-click Oblivion → Properties → Launch Options, paste:

```
WINEDLLOVERRIDES="obse_steam_loader=n,b" %command%
```

**Windows users skip this step.**

---

## Step 5 — Launch

Launch Oblivion through Steam normally. Click **Play** in the launcher.

Load a save or start a new game. Once the game world loads, a login dialog will appear automatically.

- **First time:** choose Register → enter a username, password, and character name
- **Returning:** choose Log In → enter your credentials
- **Reconnecting:** if you've logged in before, your session token is saved and you're logged in automatically

---

## In-Game

| Key | Action |
|-----|--------|
| F10 | Open command input window |

### Commands

```
/players                  — list online players
/who                      — show your character info
/tell <name> <message>    — private message
/party invite <name>      — invite to party
/party accept <name>      — accept a party invite
/party leave              — leave your party
/party members            — list party members
/setpw <old> <new>        — change your password
/help                     — full command list
```

---

## Troubleshooting

**Login dialog never appears**
- Make sure the server is running and reachable on port 7777
- Check `Oblivion/obse_loader.log` — if it mentions Steam, the Linux patches weren't applied
- Make sure `TES4MP.dll` is in `Data/OBSE/Plugins/`

**Oblivion crashes after intro videos (Linux)**
- Try Proton 7.0 or GE-Proton7-55 via ProtonUp-Qt
- Avoid Proton Experimental for Oblivion

**Session expired on reconnect**
- Session tokens last 24 hours — just log in again with your password
