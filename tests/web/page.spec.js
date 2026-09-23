// Device page against tests/web/mock_device.py. Each test resets the mock,
// optionally with state overrides, and checks what the page posts back.
const { test, expect } = require("@playwright/test");

const reset = async (request, state) => {
  const q = state ? "?state=" + encodeURIComponent(JSON.stringify(state)) : "";
  await request.post("/_reset" + q);
};
const posts = async (request) => (await (await request.get("/_log")).json()).filter((p) => p.path === "/gameday/set");

test("fallback setting renders from fallback_mode and posts the set key", async ({ page, request }) => {
  await reset(request, { mode: 2 });
  await page.goto("/");
  const sw = page.locator("#fallbackSw");
  await expect(sw).not.toHaveClass(/on/);
  await expect(page.locator("#liveHint")).toContainText("the next kickoff");
  await sw.click();
  await expect.poll(async () => (await posts(request)).map((p) => p.q.fallback)).toContain("my_team");
  await expect(page.locator("#liveHint")).toContainText("your team's game");
  await expect(sw).toHaveClass(/on/);
  await sw.click();
  await expect.poll(async () => (await posts(request)).map((p) => p.q.fallback)).toContain("next_game");
});

test("fallback switch starts on for a panel set to my_team", async ({ page, request }) => {
  await reset(request, { fallback_mode: "my_team" });
  await page.goto("/");
  await expect(page.locator("#fallbackSw")).toHaveClass(/on/);
});

test("a live mode with no game shows the device's own words", async ({ page, request }) => {
  await reset(request, { mode: 2, status: "No college games scheduled", game: { s: "NOT_FOUND", l: "ncaa" } });
  await page.goto("/");
  await expect(page.locator("#msg")).toHaveText("No college games scheduled");
});

test("idle screens card renders the bitmask and posts a new one", async ({ page, request }) => {
  await reset(request, { idle_screens: 1 | 4 });
  await page.goto("/");
  const sw = (i) => page.locator(`.sw[data-idle="${i}"]`);
  await expect(sw(0)).toHaveClass(/on/);
  await expect(sw(1)).not.toHaveClass(/on/);
  await expect(sw(2)).toHaveClass(/on/);
  await sw(4).click();  // weather on: clock + standings + weather
  await expect.poll(async () => (await posts(request)).map((p) => p.q.idle)).toContain(String(1 | 4 | 16));
  await sw(0).click();  // clock off
  await expect.poll(async () => (await posts(request)).map((p) => p.q.idle)).toContain(String(4 | 16));
});

test("idle rotate and off-after post their keys", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#idlerot")).toHaveValue("60");
  await page.locator("#idlerot").fill("5");
  await page.locator("#idlerot").press("Enter");
  await expect.poll(async () => (await posts(request)).map((p) => p.q.idlerot)).toContain("10");  // clamped
  await page.locator("#idleoff").selectOption("30");
  await expect.poll(async () => (await posts(request)).map((p) => p.q.idleoff)).toContain("30");
});

test("idle state shows on the board", async ({ page, request }) => {
  await reset(request, { idle: true, idle_screen: "countdown", game: { s: "NOT_FOUND", l: "nfl" } });
  await page.goto("/");
  await expect(page.locator("#msg")).toContainText("countdown screen");
});

test("weather location: typed, saved, cleared", async ({ page, request }) => {
  await reset(request);
  await page.goto("/");
  await expect(page.locator("#wxline")).toHaveText("No location set");
  await page.locator("#wxlat").fill("32.953712");
  await page.locator("#wxlon").fill("-96.89");
  await page.locator("#wxsave").click();
  await expect.poll(async () => (await posts(request)).map((p) => p.q.wxlat + "," + p.q.wxlon)).toContain("32.9537,-96.8900");
  await expect(page.locator("#wxline")).toContainText("Saved");
  await page.locator("#wxlat").fill("123");
  await page.locator("#wxsave").click();
  await expect(page.locator("#toast")).toContainText("Latitude -90 to 90");
  await page.locator("#wxclear").click();
  await expect.poll(async () => (await posts(request)).filter((p) => p.q.wxlat === "none" && p.q.wxlon === "none").length).toBe(1);
  await expect(page.locator("#wxline")).toHaveText("No location set");
});

test("weather location outside the US says so", async ({ page, request }) => {
  await reset(request, { wx_lat: 51.5072, wx_lon: -0.1276, wx_grid: "-" });
  await page.goto("/");
  await expect(page.locator("#wxline")).toContainText("US only");
  await expect(page.locator("#wxlat")).toHaveValue("51.5072");
});

test("phone location fills and saves the weather location", async ({ browser, request }) => {
  await reset(request);
  const context = await browser.newContext({
    geolocation: { latitude: 32.95371, longitude: -96.89032 },
    permissions: ["geolocation"],
  });
  const page = await context.newPage();
  await page.goto("/");
  await page.locator("#wxgeo").click();
  await expect.poll(async () => (await posts(request)).map((p) => p.q.wxlat + "," + p.q.wxlon)).toContain("32.9537,-96.8903");
  await expect(page.locator("#wxlon")).toHaveValue("-96.8903");
  await context.close();
});
