# Game Day Scoreboard

A standalone football scoreboard for the [Apollo Automation M-1](https://wiki.apolloautomation.com/) HUB75 matrix. Install it from your browser, join it to WiFi, pick your NFL or college team on the panel's own web page, and it follows the game from ESPN by itself. Nothing else to run.

If you already use Home Assistant and want room lighting to celebrate too, [gameday-matrix](https://github.com/bharvey88/gameday-matrix) does the same thing through a blueprint.

## What it does

- Next-game card while you wait: both logos, season records, kickoff day and time in your timezone, and a ticker with the betting line, over/under, TV network and venue
- Live scoreboard at kickoff: scores, the game clock and quarter in large type under them, down and distance beneath that (red in the red zone), abbreviations in team colors with the team holding the ball in gold, records under the logos, timeout pips, and a scrolling ticker with the last play (clock and down and distance can be added to the ticker too)
- Full-screen splashes in your team's color for touchdowns, field goals, extra points and two-point conversions, the opponent's scores acknowledged in theirs, and a victory splash on a win
- Lingers on the final for half an hour, then looks up the next game
- Live modes that follow a random game in progress (NFL, college, or either) and move on when it ends
- A device page that mirrors the panel, celebrates with it, lists the next three games, and holds every setting
- Four favorite teams on a WizMote remote's numbered buttons
- Updates itself over WiFi from the device page when a new release is out
- Adapts to one 64x64 panel (logos side by side, stacked scores) or two panels wide (logos at the edges, big scores, records and pips in the middle); one firmware, the count is a setting

## Hardware

- Apollo M-1 (rev 6) controller
- One 64x64 HUB75 panel, or two side by side for a 128x64 display (one firmware; pick the count on the device page)

## Install

1. Open the [installer page](https://bharvey88.github.io/gameday-scoreboard/) in Chrome or Edge, plug the M-1 in over USB, and click Install.
2. Enter your WiFi details in the dialog that follows the install.
3. Once it's on WiFi the panel says where to go: open the `gameday-xxxxxx.local` name it shows on your phone, or the address underneath if your phone can't resolve `.local` names. Pick your team on that page. The ticker repeats the address until you've chosen a team, and holding the M-1's boot button for 1.5 seconds shows it again any time.

Team changes take effect immediately and survive reboots. Two panels side by side? Click the two-panel picture in the Setup card. Nothing needs reflashing to change teams or layouts.

## The device page

The M-1 serves its own page: a live board that mirrors the panel (logos, score, clock, down and distance, timeout pips, last play), a team chooser with logos and search plus an "On now" tab listing today's NFL and FBS games straight from ESPN (tap the side you want to follow), and the settings grouped in plain language. It is plain HTML and JavaScript embedded in the firmware, so it works with no internet beyond the logos. The page reads one JSON document from the device (`GET /gameday/state`) and writes settings back to `POST /gameday/set`, so it is not limited to what an ESPHome entity can carry.

| Setting | What it does |
| --- | --- |
| Show | My team, or a live game picked at random from the NFL, college, or both, switching every few minutes |
| Team | One list of all 32 NFL teams and every FBS college team, plus an "On now" tab |
| Timezone | Follows the browser's zone unless you pick one by hand; used for kickoff times |
| Ticker: down and distance, last play, odds and TV | Choose what scrolls along the bottom |
| Opponent scores too | Turn the opponent's scoring splashes off if you only want yours |
| Favorites 1 to 4 | Teams for the numbered buttons on a WizMote remote |
| Remote | Pair a WizMote: turn discovery on, press any button on the remote. ON/OFF, brightness and NIGHT buttons control the panel |
| Panels | One 64x64 panel or two side by side. Changing it restarts the device |
| Firmware / Check for updates | The device checks this project's releases every 6 hours; an Install button appears on the page when a newer version exists and updates over WiFi |
| Brightness, Panel on, Ticker speed, Showing | Panel brightness, panel on or off, ticker speed, scoreboard or clock page |
| Refresh scores, Reboot | Re-fetch right away, restart |

## Home Assistant is optional

The device runs on its own, but it is a normal ESPHome device. If Home Assistant is on the same network it will discover it with the controls worth automating: Team, Mode, Power, Brightness, Scroll Speed, Select Page, Game Status, Last Play, Firmware, Refresh Now, Reboot and the WizMote pairing switches. Favorites, timezone, ticker options, splashes and the panel count are set from the device page only. The API has no encryption key in the prebuilt binary; build from YAML if you want one.

## Build it yourself

```
git clone https://github.com/bharvey88/gameday-scoreboard
cd gameday-scoreboard/firmware
esphome run gameday.yaml
```

`gameday.yaml` only names the build; everything lives in `gameday-common.yaml`. The panel count is a saved preference applied at boot by `components/panel_layout`, so one binary serves both layouts. The scoreboard page is in `pages/gameday-live.yaml`, the ESPN logic in `components/gameday`, and the device web page in `firmware/web` (edit `app.js` or `app.css`, then run `python scripts/build_web.py` to refresh the embedded bundle). The controller, theme and clock packages come from [hub75-studio](https://github.com/pavlov-net/hub75-studio), pinned to a commit.

## How it works

The `gameday` component asks ESPN's public site API for the team's next event, then polls that day's scoreboard: every 15 minutes while the game is far off, every minute inside the last hour, every 5 seconds during the game, and every minute for half an hour after the final. The JSON is streamed through a filter on the device so a 200KB scoreboard never has to fit in memory at once. Score deltas between polls decide the splashes, the same rules the blueprint uses. Logos are ESPN's dark-background PNGs, decoded and shrunk to 32x32 on the ESP32.

The parser and game logic have host tests: `make -C tests` (needs g++). To refresh the team list at the start of a season, run `python scripts/build_teams.py` and commit the regenerated header.

## Notes

- ESPN's API is unofficial and can change without notice. The device sends a curl-style User-Agent because ESPN's edge rejects unfamiliar ones.
- All HTTPS requests (ESPN, logos, firmware updates) are certificate-verified against the ESP-IDF bundle. If ESPN ever moves to a certificate authority outside that bundle, fetches will fail until a rebuild.
- Team logos are ESPN's and are fetched at runtime, not bundled.
- Fetching runs on its own task, so the panel animation keeps going while a document downloads.

## Credits

Built on hub75-studio's LVGL pages and controller packages, and on the attribute model of the ha-teamtracker integration. Thanks to both projects.

## License

MIT.
