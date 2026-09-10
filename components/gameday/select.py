import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE

from . import GamedayComponent, gameday_ns, team_options

CONF_GAMEDAY_ID = "gameday_id"

GamedaySelect = gameday_ns.class_("GamedaySelect", select.Select, cg.Component)
SelectType = gameday_ns.enum("SelectType", is_class=True)
# Favorites and the timezone are set from the device page (/gameday/set) and
# have no entity; only the two selects worth having in Home Assistant remain.
SELECT_TYPES = {
    "team": SelectType.TEAM,
    "mode": SelectType.MODE,
}
MODE_OPTIONS = ["My team", "Live NFL", "Live college", "Live anything", "Favorite teams"]  # must match gameday.cpp

CONFIG_SCHEMA = (
    select.select_schema(GamedaySelect)
    .extend(
        {
            cv.GenerateID(CONF_GAMEDAY_ID): cv.use_id(GamedayComponent),
            cv.Required(CONF_TYPE): cv.one_of(*SELECT_TYPES, lower=True),
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
    options = team_options() if kind == "team" else MODE_OPTIONS
    await select.register_select(var, config, options=options)
    cg.add(var.set_type(SELECT_TYPES[kind]))
    cg.add(var.set_parent(parent))
    if kind == "team":
        cg.add(parent.set_team_select(var))
    else:
        cg.add(parent.set_mode_select(var))
