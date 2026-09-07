# Changelog

Each release has a section here. The release workflow copies the section for
the tagged version into the GitHub release and into the firmware manifest, so
the device page shows it when an update is available. A tag without a
matching section fails the build.

Format: `## vX.Y.Z` heading, then short bullets written for the person who
owns the panel, not for the code.

## v0.2.5

- After installing an update from the device page, the page now reloads itself as soon as the panel is back, instead of waiting on the slow reconnect.

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
