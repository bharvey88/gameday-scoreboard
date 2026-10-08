// Device page tests against mock_device.py. `npm ci` and
// `npx playwright install chromium` once, then `npm test`.
// PYTHON picks the interpreter for the mock (default python3, python on Windows).
const { defineConfig } = require("@playwright/test");

const PORT = Number(process.env.MOCK_PORT) || 8791;

module.exports = defineConfig({
  testDir: ".",
  testMatch: /.*\.spec\.js/,
  workers: 1, // one mock, one shared state
  forbidOnly: !!process.env.CI,
  reporter: process.env.CI ? [["list"], ["github"]] : "list",
  use: {
    baseURL: `http://127.0.0.1:${PORT}`,
    browserName: "chromium",
    // Match the mock's US Central so only the timezone test sees the page post a zone
    timezoneId: "America/Chicago",
    locale: "en-US",
  },
  webServer: {
    command: `${process.env.PYTHON || (process.platform === "win32" ? "python" : "python3")} mock_device.py ${PORT}`,
    url: `http://127.0.0.1:${PORT}/gameday/state`,
    reuseExistingServer: false,
    timeout: 20000,
  },
});
