import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE

CONF_SLOT = "slot"

from . import GamedayComponent, gameday_ns, team_options, timezone_options

CONF_GAMEDAY_ID = "gameday_id"

GamedaySelect = gameday_ns.class_("GamedaySelect", select.Select, cg.Component)
SelectType = gameday_ns.enum("SelectType", is_class=True)
SELECT_TYPES = {
    "team": SelectType.TEAM,
    "timezone": SelectType.TIMEZONE,
    "mode": SelectType.MODE,
    "favorite": SelectType.FAVORITE,
}
MODE_OPTIONS = ["My team", "Live NFL", "Live college", "Live anything"]  # must match gameday.cpp

CONFIG_SCHEMA = (
    select.select_schema(GamedaySelect)
    .extend(
        {
            cv.GenerateID(CONF_GAMEDAY_ID): cv.use_id(GamedayComponent),
            cv.Required(CONF_TYPE): cv.one_of(*SELECT_TYPES, lower=True),
            cv.Optional(CONF_SLOT): cv.int_range(min=1, max=4),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GAMEDAY_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # Compare the plain string: codegen enum members are MockObj placeholders
    # and comparing them with == is always truthy.
    kind = config[CONF_TYPE]
    if kind == "team":
        options = team_options()
    elif kind == "favorite":
        if CONF_SLOT not in config:
            raise cv.Invalid("favorite selects need slot: 1-4")
        options = ["None"] + team_options()
    elif kind == "mode":
        options = MODE_OPTIONS
    else:
        options = timezone_options()
    await select.register_select(var, config, options=options)
    cg.add(var.set_type(SELECT_TYPES[kind]))
    cg.add(var.set_parent(parent))
    if kind == "team":
        cg.add(parent.set_team_select(var))
    elif kind == "favorite":
        cg.add(var.set_slot(config[CONF_SLOT]))
        cg.add(parent.set_favorite_select(config[CONF_SLOT], var))
    elif kind == "mode":
        cg.add(parent.set_mode_select(var))
    else:
        cg.add(parent.set_timezone_select(var))
