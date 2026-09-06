# Marvel's Spider-Man 2 — Archipelago (alpha)

Swing New York with your powers randomized by [Archipelago](https://archipelago.gg).
Two ways to play, chosen in your YAML:

- **Zero to Hero** — you start stripped of abilities, gadgets and suit tech. Every
  check you complete hands something back.
- **Longest Night** — you start with *everything*, and every check you complete
  takes something away. Strongest in the first hour, fighting with scraps by the
  end. Pace yourself; it's going to be a long one.

Both grant every suit up front. Districts are gated: each opens with a
`District: <name>` item, and a locked district is **enforced in game** — see
*District gates* below.

**Status: alpha.** Everything below has been run live, but nobody has finished a
full seed yet. Expect rough edges, keep the log, and report what you hit.

---

## What you need

- Marvel's Spider-Man 2 (PC, Steam)
- [Overstrike](https://github.com/Tkachov/Overstrike) — the mod installer for the game
- [Archipelago](https://archipelago.gg/) 0.6.4 or newer
- Python 3.11 or newer (from python.org or the Microsoft Store)

## Install — six steps

1. **The mod.** Put `SM2-Archipelago.script` in Overstrike's `Mods Library`,
   tick it, **Refresh**, **Install**. Untick any other SM2 script mods.
2. **The world.** Put `spiderman2.apworld` in Archipelago's `custom_worlds` folder
   (`C:\ProgramData\Archipelago\custom_worlds` on a default install). Restart the
   Launcher, generate a template YAML, edit it, generate a seed as usual.
3. **The client.** Unzip the `client` folder anywhere you like. Double-click
   **`Start Client.bat`** once. It installs the one Python package it needs and
   starts the client, which then waits for you.
4. **The save.** Archipelago needs the open world with nothing collected, and
   a new game doesn't have that. Copy `save\slot2-manual-0.save` into your save
   folder (`Documents\Marvel's Spider-Man 2\<long number>\`) -- **it replaces
   save slot 2, back yours up first**. Details in `save\README.txt`.
5. **Play.** Launch the game, load slot 2, get into gameplay.
6. **Connect.** Press **F8** in game. A small window opens — server, slot,
   password — hit **Connect**. (The password is the room's, if the host set one
   in Archipelago's `host.yaml`; otherwise leave it blank.) The dot goes green,
   the feed in the lower-left says the seed is landing, and you're playing.

You only need step 3 once per session (F8 finds the client after that) and step 4 once ever.

## What you'll see

A neon feed in the lower-left corner of the game:

- `CHECK` — a location you just completed
- `ITEM` / `LOST` — what arrived, or what the night took
- `UPDATE` — the run talking to you: district notices, the goal, your energy

A dot with the connection state sits under it. If the dot is red, press F8.

## District gates

Your YAML picks what a *locked* district does to you (`district_gate`):

| Option | What happens |
|---|---|
| `auto` (default) | Night's Grip in Longest Night, Powers Fade in Zero to Hero |
| `nights_grip` | A warning, five seconds, then your health drains to 1 and stays there until you leave. An **ENERGY** meter shows the drain — the game's own bar doesn't. You *can* dash in for a spider-bot. You cannot fight. |
| `powers_fade` | Your abilities and gadgets are taken on entry and returned when you leave. Combat missions become impossible; traversal stays. |
| `boundary` | You are put back where you came from. A wall. |
| `none` | Logic-only. Nothing enforced. |

`random_start: false` (default) makes the district you load into one of your free
ones. With it on, every starting district is random and your spawn district is
only open until you first leave it.

## Goal

`goal` in your YAML: **all_side_missions** (default — memories, nests, blinds,
mysteriums, stashes, Brooklyn Visions, EMF, FNSM), `all_spider_bots`,
`all_photo_ops`, or `everything`. The client tells the server when you're done.

## Checks

| Category | Count | Detected by |
|---|---|---|
| Marko's Memories | 12 | mission name in the save |
| Symbiote Nests | 10 | mission name |
| Hunter Blinds | 11 | mission name |
| Mysteriums | 9 | mission name |
| Prowler Stashes | 5 | mission name |
| Brooklyn Visions | 3 | mission name |
| EMF Experiments | 3 | mission name |
| FNSM Requests | 3 | mission name |
| Spider-Bots | 42 | the save's collectible block |
| Photo Ops | 23 | the save's collectible block |

Each category is a YAML toggle. Detection reads your **autosave**, so a check
lands when the game next saves — usually within a minute, immediately after a
fast-travel.

Spider-bots and photo ops are named by position — `Spider-Bot 07`, `Photo Op 12`
— because the game stores no name for them. That'll get friendlier as the
name table fills in.

## Known gaps (please don't report these)

- Nobody has finished a full seed. That's what you're for.
- Spider-bot and photo-op names are positions, not the bots' real names.
- `powers_fade` and `boundary` have had less live time than `nights_grip`.
- A few districts still resolve by position rather than the game's own
  district id; borders can be a few metres off.
- Story missions are not checks.

## When something breaks

Grab these and send them with what you were doing:

- `%LOCALAPPDATA%\SM2-Archipelago\APMod.log` (the mod)
- `%LOCALAPPDATA%\SM2-Archipelago\client-out.txt` (the client)
- your YAML and the seed's spoiler if you have it

If the game crashes, the last few lines of `APMod.log` usually say what the mod
was doing.

## Credits

Built by Heroic-Creations. The save-format and hashing work stands on
[ALERT](https://github.com/Tkachov/ALERT) and akintos' InsomniacArchive; the mod
loads through Overstrike.
