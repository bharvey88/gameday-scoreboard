# Changelog

Each release has a section here. The release workflow copies the section for
the tagged version into the GitHub release and into the firmware manifest, so
the device page shows it when an update is available. A tag without a
matching section fails the build.

Format: `## vX.Y.Z` heading, then short bullets written for the person who
owns the panel, not for the code.

## v1.1.2

- The device page's cards flow in two columns with no gaps: a short card no longer leaves a hole beside a tall one.

## v1.1.1

- The possession marker is a gold underline under the team with the ball (under the score on a single panel), so it visibly jumps from one side to the other. The first cut of it was two dots in the middle that barely moved.

## v1.1.0

- Fireworks. Touchdowns and the win now set off fireworks on the panel behind the splash text, in the team's color. Field goals and extra points keep the quick color flash.
- Possession is a gold underline under the team with the ball. Team names stay in their own colors instead of turning gold, which read like the teams had swapped.

## v1.0.1

- The firmware box shows the version that's actually running after an update, without a manual refresh.
- Cards on the device page no longer stretch to match their neighbour, so Remote is as tall as its contents.

## v1.0.0

- First full release. One firmware for one or two panels, installed from the browser, with the team, layout and every other setting on the panel's own page. No Home Assistant needed.
- Next-game card with the line, TV and kickoff; live scoreboard with clock, down and distance, possession and timeouts; full-screen celebrations; a final that lingers, then the next game.
- Device page: mirrors the panel and its celebrations, lists the next three games, holds favorites for a WizMote remote, live modes that follow any game in progress, and updates the firmware over WiFi.
- Since v0.8.4: the installer pins its loader with an integrity hash and describes the current setup flow; the firmware box no longer reads "unknown" after a reboot (the first update check ran before the clock was set); Check for updates stays quiet during an install.

## v0.8.4

- A Setup card gathers the things you set once: the matrix pictures, the timezone, and the startup line, now labelled "Show the page address when the panel starts" with a note on what it does. Device is back to firmware, refresh, reboot and diagnostics.

## v0.8.3

- The mode picker is a row of buttons above the team bar: My team, Live NFL, Live college, Any live game. In a live mode the team bar becomes a one-line explanation with a small "switch every N min" field instead of the big slider.
- Hidden things on the page now stay hidden. A styling slip let some hidden rows show anyway, which is why the rotation slider was always there.
- Release notes on the device page are a bullet list again instead of one run-on paragraph. The page fetches the full notes from GitHub when it can and falls back to the short summary in the manifest.

## v0.8.2

- The board fills in right after boot again. Since v0.8.0 the season schedule for "Up next" was downloaded before the game, which held the board on "Loading" for up to half a minute. The game comes first now and the list follows a moment later.
- The page reloads itself after an install again. It was waiting for a version field the device no longer sends in the plain state, so it sat on "Installing" after the panel had already rebooted.
- A hidden demo for showing the panel off without a live game: POST /gameday/action?do=demo plays a scripted game with real splashes for about 50 seconds, then live data resumes.
- The "Preview setup screen" button is gone from the device page. The boot button still shows the address on a long press.

## v0.8.1

- Ready for ESPHome 2026.9: the timezone picker and the "Connecting to" screen use the new APIs, and the firmware still builds on 2026.8. No visible change.

## v0.8.0

- "Up next" on the device page: the three games after the one on the board, with the opponent's logo, home or away, kickoff in your timezone and the TV network when ESPN lists one. Refreshes with the schedule every 6 hours.
- The page now celebrates too. When the panel splashes a score, the same text flashes over the board on the page in the team's color.

## v0.7.2

- The Power switch and Brightness now take effect at boot. A panel that was switched off before a power cycle came back lit at a default brightness with the page showing it off, and turning it on jumped to your brightness setting. Both are applied as soon as the display starts.

## v0.7.1

- A panel that already has a team no longer sits on the setup screen for 45 seconds after every boot. It shows its address for 3 seconds and goes straight to the scoreboard. A new panel keeps the long setup screen, and it disappears the moment a team is picked.
- New "Show address at boot" toggle on the Device card, for skipping even the 3 seconds.

## v0.7.0

- The device page talks to the scoreboard directly instead of going through Home Assistant style entities, so it is faster to react and no longer limited by the 255-character cap those entities had. Nothing looks different; team changes, favorites, timezone and toggles all land the same way.
- Slimmer in Home Assistant: the page-only settings (favorites, timezone, ticker toggles, opponent splashes, rotation, panel count, preview button, the raw Game sensor) no longer appear as entities. Team, Mode, Power, Brightness, Scroll Speed, Select Page, Game Status, Last Play, Firmware, Refresh Now, Reboot and the WizMote switches stay.

## v0.6.0

- One firmware for both layouts. The installer has a single Install button, and a Panels card on the device page shows two pictures: click the one that matches your matrix (one 64x64 panel, or two side by side) and the panel restarts using it. A fresh install starts on one panel, so the setup screen is readable on any matrix.
- If you were on the two-panel build before this update, the panel comes up on the left panel only until you click the two-panel picture once. It remembers after that.
- Team logos use ESPHome's current image platform (no change on the panel, just quieter builds).

## v0.5.3

- The network name is gameday-xxxxxx.local again, with the panel's own six-character suffix, so two panels on one network never collide. The setup screen and the ticker hint show the exact name.
- Check for updates no longer pops a message when there's nothing new; the firmware row says so.

## v0.5.2

- The panel is now reachable at gameday.local (no more six-character suffix in the name).
- The setup screen reads like instructions: "On your phone, open gameday.local, or 10.10.10.97, then pick your team", and the hotspot screen says a setup page will open after joining. The ticker hint says the same until a team is picked.

## v0.5.1

- The setup screen shows the address itself in the larger font ("http://10.10.10.97", or just the address on a single panel) with "pick your team here" under it, so nothing important scrolls.
- A "Preview setup screen" button on the Device card plays the three setup screens on the panel (hotspot, connecting, address) without touching WiFi.

## v0.5.0

- Check for updates now answers. The button reads "Checking" for a few seconds and then the page says "You're up to date" or "Update available", instead of quietly doing nothing when there's no newer version.
- A real setup screen at boot. It says "Join WiFi: Game Day Scoreboard" while the hotspot is up, "Connecting" with the network name once credentials are saved, and "Open http://<address> to pick your team" for 30 seconds once it's on WiFi, with a progress bar while it works.
- Until a team has been picked once, the ticker starts with "Setup: http://<address>" so a new panel tells you where its page is.
- Hold the M-1's boot button for 1.5 seconds and the address screen comes back for 15 seconds.

## v0.4.3

- The clock on the panel now comes from ESPN's raw clock and period instead of their pre-formatted text, which lagged a poll or two behind the down and distance. Everything on the board updates together.

## v0.4.2

- Switching teams is fast now. The panel shows the new team's name and logo immediately and fills in the game a second later, and the old opponent's logo clears instead of lingering.
- Logos come from ESPN already shrunk to 64 pixels (about 3KB instead of up to 95KB), so they load in well under a second and no longer stall the display while decoding.

## v0.4.1

- After an update from the device page, the page reloads within a few seconds of the panel coming back instead of up to 45 seconds later. It now watches the running version number rather than waiting to see the connection drop.

## v0.4.0

- WizMote remote support. Turn on discovery in the Remote card, press any button on the remote, and it's paired. ON and OFF control the panel, the brightness buttons work, NIGHT dims it.
- Four favorite teams. Set them in the Favorites card; buttons 1 to 4 on the remote switch the panel to that team (and back to My team mode if it was showing live games).

## v0.3.0

- Live game modes. A new Show dropdown on the device page: My team, Live NFL, Live college, or Live anything. In a live mode the panel picks a game in progress at random, follows it, and jumps to another when it ends or when the "switch games every" timer runs out (2 to 30 minutes, default 5).
- In live modes both teams get scoring splashes with their abbreviation, and the winner gets the final splash.
- The device page uses the width of the screen: bigger board and side-by-side settings on desktop.

## v0.2.5

- After installing an update from the device page, the page now reloads itself as soon as the panel is back, instead of waiting on the slow reconnect.
- The Time card is one line: the zone the panel is using and where it came from. The dropdown only appears if you ask to choose manually.

## v0.2.4

- The timezone now sets itself from whatever browser opens the device page. Picking a zone by hand turns that off so your choice sticks; the "Set from this browser" toggle in the Time card turns it back on.
- Shorter timezone list (US, Canada, Mexico, UK, Central Europe, Australia, UTC) and the stray "Display name" entry is gone.
- Removed the "Game clock in the ticker" toggle. The clock has its own row on the panel.

## v0.2.3

- A proper timezone list: all of North America plus the common zones worldwide.
- The device page notices the timezone your browser is in and offers it with one tap when the panel is set to something else.
- The Team and Timezone dropdowns show their real values after a reboot. A code-generation slip had wired the stored team into the Timezone dropdown and left Team blank, so the page said "No team chosen" even though the panel was following the right team.

## v0.2.2

- Fixed every control on the device page. ESPHome 2026.8 changed how its web server names entities, so team changes and toggles were silently failing with a 404. They work now.
- Over-the-air updates. The panel checks this project's releases every six hours. When a newer version exists, the Device card shows an Install button and the update runs over WiFi.
- Added a Check for Updates button.
- Every HTTPS request, including firmware downloads, is now certificate-verified.

## v0.2.1

- The team chooser has an "On now" tab: today's NFL and FBS games straight from ESPN, live games first. Tap the side you want to follow.
- Control requests from the page retry once and report the HTTP status if they fail.

## v0.2.0

- New device web page: a live board that mirrors the panel, a team chooser with logos and search, and settings grouped in plain language. Replaces the stock ESPHome entity list.
- The stored team and timezone are published again after the network comes up so the page shows the right values after a reboot.

## v0.1.3

- Kept the game clock on one line. "12:34 - 2nd" was wrapping onto the down and distance row.

## v0.1.2

- ESPN is polled every 5 seconds during a game.
- Two-panel layout: down and distance under the clock (red in the red zone), records under the logos, abbreviations in team colors with the team holding the ball in gold.
- The ticker sits on the very bottom row with a clear gap above it.

## v0.1.1

- Game clock and quarter shown in large type under the scores during the game.
- Fetching moved to its own task so the ticker keeps scrolling during downloads.
- Polling every 10 seconds during a game.
- No more reboot after 15 minutes without a Home Assistant connection.
- College teams labelled NCAAF in the team list.

## v0.1.0

- First release. Standalone football scoreboard for the Apollo M-1: flash from the browser, join WiFi, pick a team on the device page, and the panel follows the game from ESPN with no Home Assistant needed.
