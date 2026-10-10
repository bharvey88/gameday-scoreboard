// Device page (firmware/web/bundle.js) against mock_device.py. Each test
// resets the mock, optionally with state overrides, and checks what the page
// shows and what it posts back to /gameday/set.
const base = require("@playwright/test");
const { expect } = base;

// Answer every off-device request locally so the suite needs no network.
// ESPN's scoreboard feeds the picker's "On now" tab and has no games unless a test routes one.
// Uncaught page errors fail the test.
const test = base.test.extend({
  page: async ({ page, baseURL }, use) => {
    const errors = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await page.route(
      (url) => url.origin !== new URL(baseURL).origin,
      (route) => route.fulfill({ status: 404, body: "" }),
    );
    await page.route("https://site.api.espn.com/**", (route) => route.fulfill({ json: { events: [] } }));
    await use(page);
    expect(errors, "uncaught errors on the page").toEqual([]);
  },
});

const reset = (request, state, fail) => request.post("/_reset", { data: { state, fail } });
const sets = async (request) =>
  (await (await request.get("/_log")).json()).filter((p) => p.path === "/gameday/set").map((p) => p.q);
const expectSet = (request, q) => expect.poll(() => sets(request)).toContainEqual(q);

const LIVE = {
  s: "IN", ts: 14, os: 10, p: 1, tt: 2, ot: 3, rz: true,
  c: "8:12 - 2nd", d: "3rd & 4 at PHI 18", lp: "Prescott pass to Lamb for 12 yards",
};

test("shows a live game and the settings from /gameday/state", async ({ page, request }) => {
  await reset(request);
  const g = (await (await request.get("/gameday/state")).json()).game;
  await reset(request, { game: { ...g, ...LIVE }, odds: false, status: "DAL 14 PHI 10 | 8:12 2nd" });
  await page.goto("/");
  await expect(page.locator("#conn")).toHaveClass(/on/);
  await expect(page.locator("#gstate")).toHaveText("LIVE");
  await expect(page.locator("#tA .abbr")).toHaveText("DAL");
  await expect(page.locator("#tA .abbr")).toHaveClass(/poss/);
  await expect(page.locator("#tB .abbr")).toHaveText("PHI");
  await expect(page.locator("#sA")).toHaveText("14");
  await expect(page.locator("#sB")).toHaveText("10");
  await expect(page.locator("#clock")).toHaveText("8:12 - 2nd");
  await expect(page.locator("#down")).toHaveText("3rd & 4 at PHI 18");
  await expect(page.locator("#down")).toHaveClass(/rz/);
  await expect(page.locator("#tA .pips i.on")).toHaveCount(2);
  await expect(page.locator("#play")).toHaveText("Last play: Prescott pass to Lamb for 12 yards");
  await expect(page.locator("#ticker")).toHaveText("DAL 14 PHI 10 | 8:12 2nd");

  await expect(page.locator("#modes button.on")).toHaveText("My team");
  await expect(page.locator("#teambar")).toBeVisible();
  await expect(page.locator("#curName")).toHaveText("Dallas Cowboys");
  await expect(page.locator('.sw[data-set="down"]')).toHaveClass(/on/);
  await expect(page.locator('.sw[data-set="odds"]')).not.toHaveClass(/on/);
  await expect(page.locator('select[data-fav="2"]')).toHaveValue("nfl:34");
  await expect(page.locator('select[data-fav="4"]')).toHaveValue("none");
  await expect(page.locator("#panels .panelpick.on")).toHaveAttribute("data-cols", "2");
  await expect(page.locator("#tzline")).toContainText("US Central");
  await expect(page.locator("#hostname")).toHaveText("gameday-2f6a70.local");
  await expect(page.locator("#ver")).toHaveText("v1.5.4");
  // The game on the board is left out of Up next.
  await expect(page.locator("#upnextList .un .n")).toHaveText(["@ Commanders", "vs Texans"]);
});

test("an upcoming game shows kickoff and network", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#gstate")).toHaveText("UPCOMING");
  await expect(page.locator("#clock")).toHaveText("Sun 3:25 PM");
  await expect(page.locator("#down")).toHaveText("on FOX");
  await expect(page.locator("#msg")).toBeHidden();
});

test("no game renders the empty board for my team and for live modes", async ({ page, request }) => {
  await reset(request, { game: { s: "NOT_FOUND", l: "nfl" }, next: [] });
  await page.goto("/");
  await expect(page.locator("#board")).toHaveClass(/empty/);
  await expect(page.locator("#gstate")).toHaveText("NO GAME");
  await expect(page.locator("#msg")).toHaveText("No upcoming game for this team");
  await expect(page.locator("#upnext")).toBeHidden();

  await reset(request, { mode: 2, game: { s: "NOT_FOUND", l: "ncaa" }, next: [] });
  await page.reload();
  await expect(page.locator("#msg")).toHaveText("No live games right now");
  await expect(page.locator("#livebar")).toBeVisible();
});

test("a device that never answers /gameday/state leaves the page waiting, not broken", async ({ page, request }) => {
  await reset(request, {}, ["state"]);
  await page.goto("/");
  await expect(page.locator("#conn")).toHaveClass(/on/);
  await expect(page.locator("#gstate")).toHaveText("WAITING");
  await expect(page.locator("#board")).toHaveClass(/empty/);
  await expect(page.locator("#curName")).toHaveText("No team chosen");
  await expect(page.locator("#modes button")).toHaveCount(5);
  // Entity controls still come from the event stream.
  await expect(page.locator("#panelCtls .ctl")).toHaveCount(4);
});

test("a failed settings post is retried once, then reported", async ({ page, request }) => {
  await reset(request, {}, ["set"]);
  await page.goto("/");
  await expect(page.locator('.sw[data-set="opp"]')).toHaveClass(/on/);
  await page.locator('.sw[data-set="opp"]').click();
  await expect(page.locator("#toast")).toHaveText("Device did not respond (HTTP 500). Try again.");
  expect(await sets(request)).toEqual([{ opp: "0" }, { opp: "0" }]);
});

test("mode pills post the mode and swap the settings bar", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#modes button.on")).toHaveText("My team");
  await page.locator('#modes button[data-mode="2"]').click();
  await expectSet(request, { mode: "2" });
  await expect(page.locator("#livebar")).toBeVisible();
  await expect(page.locator("#teambar")).toBeHidden();

  await page.locator("#rotate").fill("45");
  await page.locator("#rotate").press("Enter");
  await expectSet(request, { rotate: "30" }); // the page clamps to 1..30
  await expect(page.locator("#rotate")).toHaveValue("30");

  await page.locator('#modes button[data-mode="0"]').click();
  await expectSet(request, { mode: "0" });
  await expect(page.locator("#teambar")).toBeVisible();
  await expect(page.locator("#livebar")).toBeHidden();
});

test("favorite teams mode shows each favorite and posts its settings", async ({ page, request }) => {
  const t = (abbr, name, id) => ({ l: "nfl", id, abbr, name });
  await reset(request, {
    mode: 4,
    shown: 2,
    locked: true,
    next: [
      { slot: 1, t: t("DAL", "Dallas Cowboys", 6), id: "1", s: "PRE", kick: 1792531500, oi: 21, oa: "PHI", ts: 0, os: 0, tv: "FOX", detail: "" },
      { slot: 2, t: t("HOU", "Houston Texans", 34), id: "5", s: "IN", kick: 1792520000, oi: 30, oa: "JAX", ts: 14, os: 10, tv: "CBS", detail: "8:12 - 2nd" },
    ],
  });
  await page.goto("/");
  await expect(page.locator("#favbar")).toBeVisible();
  await expect(page.locator("#favName")).toHaveText("Locked on Houston Texans");
  await expect(page.locator("#upnextList .un.fav .n")).toHaveText(["DAL vs PHI", "HOU vs JAX"]);
  await expect(page.locator("#upnextList .un.shown .w")).toHaveText("14 - 10 · 8:12 - 2nd");

  await page.locator("#lockon").fill("200");
  await page.locator("#lockon").press("Enter");
  await expectSet(request, { lockon: "120" });
  await page.locator("#release").selectOption("300");
  await expectSet(request, { release: "300" });
  await expect(page.locator("#favrotate")).toBeHidden();
  await page.locator("#collide").selectOption("1");
  await expectSet(request, { collide: "1" });
  await expect(page.locator("#favrotate")).toBeVisible();
  await page.locator("#favrotate").fill("5");
  await page.locator("#favrotate").press("Enter");
  await expectSet(request, { rotate: "5" });
});

test("the team picker searches the team list and posts the team", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#curName")).toHaveText("Dallas Cowboys");
  await page.locator("#pick").click();
  await expect(page.locator("#modal")).toHaveClass(/open/);
  await expect(page.locator(".tabs button.on")).toHaveText("NFL");
  await page.locator("#q").fill("eagles");
  await expect(page.locator("#tiles .tile .n")).toContainText(["Philadelphia Eagles", "Boston College Eagles"]);
  await page.locator("#tiles .tile", { hasText: "Philadelphia Eagles" }).click();
  await expectSet(request, { team: "nfl:21" });
  await expect(page.locator("#modal")).not.toHaveClass(/open/);
  await expect(page.locator("#curName")).toHaveText("Philadelphia Eagles");
});

test("the picker opens on today's live games and follows a side", async ({ page, request }) => {
  const side = (homeAway, id, abbr, name, score) => ({ homeAway, score, team: { id, abbreviation: abbr, shortDisplayName: name } });
  await page.route("https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard*", (route) =>
    route.fulfill({
      json: {
        events: [{
          id: "401772001",
          date: "2026-10-11T20:25Z",
          status: { type: { state: "in", shortDetail: "8:12 - 2nd" } },
          competitions: [{ competitors: [side("home", "6", "DAL", "Cowboys", "14"), side("away", "21", "PHI", "Eagles", "10")] }],
        }],
      },
    }),
  );
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#curName")).toHaveText("Dallas Cowboys");
  await page.locator("#pick").click();
  await expect(page.locator(".tabs button.on")).toHaveText("On now");
  await expect(page.locator("#tiles .gh")).toHaveText(["Live now"]);
  await expect(page.locator("#tiles .side.on .n")).toHaveText("Cowboys");
  await page.locator("#tiles .side", { hasText: "Eagles" }).click();
  await expectSet(request, { team: "nfl:21" });
  await expect(page.locator("#modal")).not.toHaveClass(/open/);
});

test("ticker, celebration and startup switches post 0 and 1", async ({ page, request }) => {
  await reset(request, { play: false });
  await page.goto("/");
  await expect(page.locator('.sw[data-set="down"]')).toHaveClass(/on/);
  await page.locator('.sw[data-set="down"]').click();
  await expectSet(request, { down: "0" });
  await page.locator('.sw[data-set="play"]').click();
  await expectSet(request, { play: "1" });
  await page.locator('.sw[data-set="bootaddr"]').click();
  await expectSet(request, { bootaddr: "0" });
  await expect(page.locator('.sw[data-set="down"]')).not.toHaveClass(/on/);
  await expect(page.locator('.sw[data-set="play"]')).toHaveClass(/on/);
});

test("favorites post one slot on change and all four on reorder", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator('select[data-fav="1"]')).toHaveValue("nfl:6");
  await page.locator('select[data-fav="4"]').selectOption("nfl:21");
  await expectSet(request, { fav4: "nfl:21" });
  await page.locator('#favCtls button[data-move="1,1"]').click();
  await expectSet(request, { fav1: "nfl:34", fav2: "nfl:6", fav3: "ncaa:145", fav4: "nfl:21" });
  await expect(page.locator('select[data-fav="1"]')).toHaveValue("nfl:34");
  await expect(page.locator('select[data-fav="2"]')).toHaveValue("nfl:6");
});

test("timezone follows the browser, or a zone picked by hand", async ({ page, request }) => {
  await reset(request, { tz: 0, tz_name: "US Eastern", tz_auto: true });
  await page.goto("/");
  await expectSet(request, { tz: "1" }); // the browser is pinned to America/Chicago
  await expect(page.locator("#toast")).toHaveText("Timezone set to US Central from this browser");
  await expect(page.locator("#tzline")).toContainText("US Central · set from this browser");

  await page.locator("#tzline a", { hasText: "choose manually" }).click();
  await expect(page.locator("#tzpick")).toBeVisible();
  await page.locator("#tzsel").selectOption({ label: "US Pacific" });
  await expectSet(request, { tzauto: "0", tz: "4" });
  await expect(page.locator("#tzline")).toContainText("US Pacific · chosen by hand");
});

test("the panel picker posts the new layout", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#panels .panelpick.on")).toHaveAttribute("data-cols", "2");
  await expect(page.locator("#panels .panelpick.on")).toContainText("One 128x64 panel");
  await page.locator('#panels .panelpick[data-cols="1"]').click();
  await expectSet(request, { panels: "1" });
  await expect(page.locator("#toast")).toHaveText("Restarting as 64x64");
  await expect(page.locator('#panels .panelpick[data-cols="2"]')).toBeDisabled();
});

test("the stale tag follows the device's consecutive misses", async ({ page, request }) => {
  await reset(request, { misses: 2 });
  await page.goto("/");
  await expect(page.locator("#gstate")).toHaveText("UPCOMING");
  await expect(page.locator("#stale")).toBeHidden();

  // Three misses is where the firmware adds "no update" to the ticker.
  await reset(request, { misses: 3, stale_s: 600 });
  await page.reload();
  await expect(page.locator("#gstate")).toHaveText("UPCOMING");
  await expect(page.locator("#stale")).toBeVisible();

  // The ticker warns with no game too, so the empty board shows it.
  await reset(request, { misses: 5, game: { s: "NOT_FOUND", l: "nfl" }, next: [] });
  await page.reload();
  await expect(page.locator("#gstate")).toHaveText("NO GAME");
  await expect(page.locator("#stale")).toBeVisible();

  await reset(request, { misses: 0, game: { s: "NOT_FOUND", l: "nfl" }, next: [] });
  await page.reload();
  await expect(page.locator("#gstate")).toHaveText("NO GAME");
  await expect(page.locator("#stale")).toBeHidden();
});
