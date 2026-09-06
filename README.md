# Game Day Scoreboard

Standalone football scoreboard firmware for the [Apollo Automation M-1](https://wiki.apolloautomation.com/) HUB75 matrix. Flash it from your browser, join it to WiFi, pick your NFL or college team on the device's own web page, and the panel follows the game from ESPN by itself. No Home Assistant, no blueprint, nothing else to run.

It is the standalone cousin of [gameday-matrix](https://github.com/bharvey88/gameday-matrix), which does the same thing through a Home Assistant blueprint and adds room lighting celebrations.

## What it does

- Next-game card while you wait: both logos, season records, kickoff day and time in your timezone, and a ticker with the betting line, over/under, TV network and venue
- Live scoreboard at kickoff: scores, the game clock and quarter in large type under them, down and distance beneath that (red in the red zone), abbreviations in team colors with the team holding the ball in gold, records under the logos, timeout pips, and a scrolling ticker with the last play (clock and down and distance can be added to the ticker too)
- Full-screen splashes in your team's color for touchdowns, field goals, extra points and two-point conversions, the opponent's scores acknowledged in theirs, and a victory splash on a win
- Lingers on the final for half an hour, then looks up the next game
- Adapts to one 64x64 panel (logos side by side, stacked scores) or two panels wide (logos at the edges, big scores, records and pips in the middle)

## Hardware

- Apollo M-1 (rev 6) controller
- One 64x64 HUB75 panel, or two side by side for a 128x64 display

## Install

1. Open the [installer page](https://bharvey88.github.io/gameday-scoreboard/) in Chrome or Edge, plug the M-1 in over USB, and pick the button for your panel layout.
2. Enter your WiFi details in the dialog that follows the install.
3. Open the device's web page (the dialog offers a link, or browse to the address your router gave it) and choose your team in the Team dropdown. Set your timezone while you are there.

Team changes take effect immediately and survive reboots. Nothing needs reflashing to change teams.

## Controls on the device page

| Control | What it does |
| --- | --- |
| Team | One list of all 32 NFL teams and every FBS college team |
| Timezone | US zones plus UTC, UK and Central Europe; used for kickoff times |
| Ticker: Game Clock, Down and Distance, Last Play, Odds and TV | Choose what scrolls along the bottom |
| Opponent Splashes | Turn the opponent's scoring splashes off if you only want yours |
| Refresh Now | Re-fetch the schedule and game right away |
| Brightness, Power, Scroll Speed | Panel brightness, panel on or off, ticker speed |
| Select Page | Switch between the scoreboard and a clock page |

## Home Assistant is optional

The device runs on its own, but it is a normal ESPHome device. If Home Assistant is on the same network it will discover it, and every control above appears as an entity. The API has no encryption key in the prebuilt binary; build from YAML if you want one.

## Build it yourself

```
git clone https://github.com/bharvey88/gameday-scoreboard
cd gameday-scoreboard/firmware
esphome run gameday-128x64.yaml
```

The two variant files only differ in panel count. Everything else lives in `gameday-common.yaml`, the scoreboard page in `pages/gameday-live.yaml`, and the ESPN logic in `components/gameday`. The controller, theme and clock packages come from [hub75-studio](https://github.com/pavlov-net/hub75-studio), pinned to a commit.

## How it works

The `gameday` component asks ESPN's public site API for the team's next event, then polls that day's scoreboard: every 15 minutes while the game is far off, every minute inside the last hour, every 5 seconds during the game, and every minute for half an hour after the final. The JSON is streamed through a filter on the device so a 200KB scoreboard never has to fit in memory at once. Score deltas between polls decide the splashes, the same rules the blueprint uses. Logos are ESPN's dark-background PNGs, decoded and shrunk to 32x32 on the ESP32.

The parser and game logic have host tests: `make -C tests` (needs g++). To refresh the team list at the start of a season, run `python scripts/build_teams.py` and commit the regenerated header.

## Notes

- ESPN's API is unofficial and can change without notice. The device sends a curl-style User-Agent because ESPN's edge rejects unfamiliar ones.
- TLS certificate verification is off for the ESPN requests so the prebuilt binary keeps working when their certificate chain rotates. Scores are public data.
- Team logos are ESPN's and are fetched at runtime, not bundled.
- Fetching runs on its own task, so the panel animation keeps going while a document downloads.

## Credits

Built on hub75-studio's LVGL pages and controller packages, and on the attribute model of the ha-teamtracker integration. Thanks to both projects.

## License

MIT.
