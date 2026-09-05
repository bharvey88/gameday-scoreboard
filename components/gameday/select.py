import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE

from . import GamedayComponent, gameday_ns, team_options, timezone_options

CONF_GAMEDAY_ID = "gameday_id"

GamedaySelect = gameday_ns.class_("GamedaySelect", select.Select, cg.Component)
SelectType = gameday_ns.enum("SelectType", is_class=True)
SELECT_TYPES = {
    "team": SelectType.TEAM,
    "timezone": SelectType.TIMEZONE,
}

CONFIG_SCHEMA = (
    select.select_schema(GamedaySelect)
    .extend(
        {
            cv.GenerateID(CONF_GAMEDAY_ID): cv.use_id(GamedayComponent),
            cv.Required(CONF_TYPE): cv.enum(SELECT_TYPES, lower=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GAMEDAY_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    kind = config[CONF_TYPE]
    options = team_options() if kind == SelectType.TEAM else timezone_options()
    await select.register_select(var, config, options=options)
    cg.add(var.set_type(kind))
    cg.add(var.set_parent(parent))
    if kind == SelectType.TEAM:
        cg.add(parent.set_team_select(var))
    else:
        cg.add(parent.set_timezone_select(var))
