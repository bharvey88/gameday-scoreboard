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
