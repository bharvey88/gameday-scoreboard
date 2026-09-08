# Changelog

Each release has a section here. The release workflow copies the section for
the tagged version into the GitHub release and into the firmware manifest, so
the device page shows it when an update is available. A tag without a
matching section fails the build.

Format: `## vX.Y.Z` heading, then short bullets written for the person who
owns the panel, not for the code.

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
