"""gameday: polls ESPN for one team's football game and hands the page
the same fields the Home Assistant blueprint used to push."""

import re
from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import http_request, runtime_image, time, web_server_base
from esphome.components.panel_layout import PanelLayout
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID, CONF_TRIGGER_ID, __version__ as ESPHOME_VERSION

DEPENDENCIES = ["network", "http_request", "time"]
AUTO_LOAD = ["json", "runtime_image", "select", "web_server_base"]
CODEOWNERS = ["@bharvey88"]

CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_PANEL_LAYOUT_ID = "panel_layout_id"
CONF_ON_UPDATE = "on_update"
CONF_ON_ACTION = "on_action"

gameday_ns = cg.esphome_ns.namespace("gameday")
GamedayComponent = gameday_ns.class_("GamedayComponent", cg.Component)
UpdateFields = gameday_ns.struct("UpdateFields")
UpdateFieldsConstRef = UpdateFields.operator("const").operator("ref")
UpdateTrigger = gameday_ns.class_(
    "UpdateTrigger", automation.Trigger.template(UpdateFieldsConstRef)
)
ActionTrigger = gameday_ns.class_(
    "ActionTrigger", automation.Trigger.template(cg.std_string)
)

COMPONENT_DIR = Path(__file__).resolve().parent

_TEAM_RE = re.compile(r'\{League::(\w+),\s*(\d+),\s*"([^"]*)",\s*"([^"]*)",\s*(\d+)\}')
# kLeagues rows in leagues.h: {League::NFL, "nfl", "football/nfl", "NFL", ...
_LEAGUE_RE = re.compile(r'\{League::(\w+),\s*"(\w+)",\s*"[^"]+",\s*"([^"]+)",')
_TZ_RE = re.compile(r'\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\}')
# Leagues left out of Home Assistant's team select. Men's college
# basketball's ~365 teams would push the options past Home Assistant's 16 KB
# attribute limit; the device page lists them.
_HA_SKIP_LEAGUES = {"MCBB"}


def team_options():
    """Select options in kTeams order, without _HA_SKIP_LEAGUES."""
    prefixes = {
        lg: prefix
        for lg, _key, prefix in _LEAGUE_RE.findall(
            (COMPONENT_DIR / "leagues.h").read_text(encoding="utf-8")
        )
    }
    text = (COMPONENT_DIR / "teams.h").read_text(encoding="utf-8")
    opts = []
    for league, _tid, _abbr, name, _group in _TEAM_RE.findall(text):
        if league in _HA_SKIP_LEAGUES:
            continue
        opts.append(f"{prefixes[league]}: {name}")
    if len(opts) < 100:
        raise cv.Invalid("teams.h looks truncated; run scripts/build_teams.py")
    return opts


def timezone_options():
    text = (COMPONENT_DIR / "timezones.h").read_text(encoding="utf-8")
    text = "\n".join(ln for ln in text.splitlines() if not ln.lstrip().startswith("//"))
    opts = [name for name, _posix, _iana in _TZ_RE.findall(text)]
    if not opts:
        raise cv.Invalid("timezones.h has no entries")
    return opts


def _parsed_timezone_count():
    text = (COMPONENT_DIR / "timezones_parsed.h").read_text(encoding="utf-8")
    return len(re.findall(r"^\s*\{-?\d+, -?\d+, \{", text, re.M))


def _esphome_at_least(year, month):
    m = re.match(r"(\d+)\.(\d+)", ESPHOME_VERSION)
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (year, month)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GamedayComponent),
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(
            http_request.HttpRequestComponent
        ),
        cv.GenerateID(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Optional(CONF_PANEL_LAYOUT_ID): cv.use_id(PanelLayout),
        cv.Optional(CONF_ON_UPDATE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(UpdateTrigger)}
        ),
        cv.Optional(CONF_ON_ACTION): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ActionTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if _parsed_timezone_count() != len(timezone_options()):
        raise cv.Invalid(
            "timezones_parsed.h is out of date; run scripts/build_timezones.py"
        )
    if _esphome_at_least(2026, 9):
        # 2026.9 removed RealTimeClock::set_timezone and the on-device POSIX
        # parser; the clock takes a pre-parsed struct instead.
        cg.add_define("GAMEDAY_TZ_PARSED")
    runtime_image.enable_format("PNG")  # logo_cache.cpp decodes ESPN's PNG logos
    cg.add(var.set_http(await cg.get_variable(config[CONF_HTTP_REQUEST_ID])))
    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(
        var.set_web_server_base(
            await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
        )
    )
    if CONF_PANEL_LAYOUT_ID in config:
        cg.add(
            var.set_panel_layout(await cg.get_variable(config[CONF_PANEL_LAYOUT_ID]))
        )
    for conf in config.get(CONF_ON_UPDATE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(UpdateFieldsConstRef, "x")], conf
        )
    for conf in config.get(CONF_ON_ACTION, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)
