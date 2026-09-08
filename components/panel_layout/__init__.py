"""panel_layout: picks the HUB75 chain length at boot from a saved preference,
so one firmware serves a single 64x64 panel and two panels side by side."""

import esphome.codegen as cg
from esphome.components.hub75 import display as hub75_display
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@bharvey88"]

CONF_DISPLAY_ID = "display_id"
CONF_DEFAULT_COLS = "default_cols"
CONF_MAX_COLS = "max_cols"

panel_layout_ns = cg.esphome_ns.namespace("panel_layout")
PanelLayout = panel_layout_ns.class_("PanelLayout", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PanelLayout),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(hub75_display.HUB75Display),
        cv.Optional(CONF_DEFAULT_COLS, default=1): cv.int_range(min=1, max=8),
        cv.Optional(CONF_MAX_COLS, default=2): cv.int_range(min=1, max=8),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    cg.add(var.set_default_cols(config[CONF_DEFAULT_COLS]))
    cg.add(var.set_max_cols(config[CONF_MAX_COLS]))
