# lr_maps (Plugify)

[LR] ExStats Maps: per-(SteamID, map) statistics in the `<lr_table>_maps` table (map plays, kills, deaths, round counters, bomb planted/defused, hostages rescued/killed, playtime on the map), saved at disconnect or after every round (lr_db_savedata_player_mode) and zeroed on !reset. Ported from the Pisex Levels Ranks module LR_ExStatsMaps. Owns its plgdb connection with the levels_ranks database settings. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_ExStatsMaps ([LR] ExStats Maps)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), levels_ranks (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `lr_maps-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_maps`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

