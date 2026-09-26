# Game Day Scoreboard firmware

ESPHome firmware for a standalone football scoreboard on an ESP32-S3 HUB75
controller (MoonHub75 pinout). Sibling repo: the iOS companion app at
~/development/gameday-scoreboard-ios (github.com/bharvey88/gameday-scoreboard-ios).

## Hard rules

- Commit as `bharvey88 <8107750+bharvey88@users.noreply.github.com>`. No
  Claude credit or Co-Authored-By lines on commits or PRs. Multiline commit
  messages via `git commit -F <file>`.
- This repo is PUBLIC. Never commit handoffs, session notes, specs, plans
  or reviews here: only product code, CHANGELOG, README and the site.
  Session notes live in the private app repo, in
  ~/development/gameday-scoreboard-ios/notes/firmware-HANDOFF.md; read it
  first and update it there at the end of every session.
- Every parallel agent works in its own git worktree.
- Product is not part of Apollo Automation. Brand is "Game Day Scoreboard".
- Decisions not to reopen without Brandon: OTA with no password, `api:` with
  no key, the device web page open ("panel trusts the home network"). Do not
  raise LAN hardening again. Prioritize customer-visible failures.
- Bump `version` in firmware/gameday-common.yaml and add a CHANGELOG.md
  section for every release; a tag without a matching section fails CI.
- ESPHome venv at ~/development/tools/esphome-venv flashes over USB and
  streams logs; Brandon's panel is gameday-2f6a70.local. Use `esphome
  compile`, not `esphome config`, to catch C++ errors. Host tests: `make -C
  tests`. Rebuild the web bundle with the venv's python (bare python3 on
  this Mac is 3.9.6, too old) after editing firmware/web/app.js.

@../gameday-scoreboard-ios/notes/firmware-HANDOFF.md
