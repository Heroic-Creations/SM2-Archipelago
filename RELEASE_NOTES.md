# v0.2.2-alpha — checks that never sent, and a wandering overlay

Two fixes from the first live multiworld.

**Hunter blinds, symbiote nests, mysteriums, FNSM requests, Brooklyn Visions
and EMF experiments never sent a check.** The client detected missions by
counting how often their name appears in the save, and that only works for
memories and stashes. Everything else has a proper mission record, and the
client now reads that record's own state. On its first scan after connecting
it also sends anything you finished while it wasn't looking, so a run that
lost checks to this gets them back by restarting the client.

**The in-game feed walked off the right edge of the screen** whenever the
ENERGY meter was showing. It was anchoring itself to its own window. Fixed
in the mod.

Install: replace the `.script` (Overstrike: Refresh, Install), the
`.apworld`, and the `client\` folder. Start the client from the new folder
once so F8 finds it.

Known and not fixed yet: the location list is short. Falcon drone chases,
six of the nine EMF experiments, hunter bases, three prowler stashes, two
FNSM requests and the three set-completion finales are not checks yet.
They're coming in the next world version, which means a new seed.

# v0.2.1-alpha — runtime folder moved to C:\ProgramData

Fixes the first-install failure where **F8 did nothing** after `Start Client.bat`.

The mod and the client meet through files in a shared folder. That folder was
under `AppData\Local`, and the Microsoft Store build of Python (what Windows 11
gives you by default) silently redirects its writes there into a private copy
the mod never sees. The folder is now `C:\ProgramData\SM2-Archipelago`, which
is not redirected. Logs to send with a bug report live there too.

Install: replace all three parts — the `.script` (Overstrike: Refresh, Install),
the `.apworld`, and the `client\` folder. Nothing else changed.

Also new since 0.2.0: `LICENSE` (MIT), `THIRD-PARTY.md`, and the mod's source
under `mod\` (GPL-3.0).

# v0.2.0-alpha — first public test build

Marvel's Spider-Man 2 for Archipelago. Two modes (Zero to Hero, Longest Night),
district gating enforced in game (Night's Grip, Powers Fade, Boundary),
126 checks across ten categories, goals, and an in-game connect window on F8.

**This is an alpha.** The whole loop has been run live, but nobody has finished
a full seed. Read `README.md` before installing — six steps, ten minutes.

What's in the zip:

- `SM2-Archipelago.script` - the mod (install with Overstrike)
- `spiderman2.apworld` - the world (Archipelago's `custom_worlds`)
- `client\` - the client; run `Start Client.bat` once
- `save\` - a starter save: the open world with nothing collected
- `Marvel's Spider-Man 2.yaml` - a commented template
- `README.md` - the tester guide, including what to send when something breaks

Known gaps are listed in the README. Please report against those, not into them.
