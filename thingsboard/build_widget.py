"""Packs schedule_widget/ into files for the ThingsBoard widgets library:
opentherm_widgets_bundle.json (bundle "OpenTherm") and
opentherm_schedule_widget.json (the widget alone)."""
import json
from pathlib import Path

HERE = Path(__file__).parent
SRC = HERE / "schedule_widget"


def key(name, color, h):
    return {"name": name, "type": "attribute", "label": name, "color": color, "settings": {}, "_hash": h}


default_config = {
    "datasources": [{
        "type": "entity",
        "name": "",
        "entityAliasId": None,
        "filterId": None,
        "dataKeys": [key("schedule", "#2196f3", 0.31), key("mode", "#4caf50", 0.32)],
    }],
    "timewindow": {"realtime": {"timewindowMs": 60000}},
    "showTitle": True,
    "title": "Расписание котла",
    "backgroundColor": "#fff",
    "color": "rgba(0, 0, 0, 0.87)",
    "padding": "8px",
    "settings": {},
    "dropShadow": True,
    "enableFullscreen": True,
    "showLegend": False,
    "widgetStyle": {},
    "titleStyle": {"fontSize": "16px", "fontWeight": 400},
    "actions": {},
}

widget = {
    "fqn": "opentherm_schedule",
    "name": "OpenTherm: расписание котла",
    "deprecated": False,
    "image": None,
    "description": "Недельное расписание платы OpenTherm: просмотр (шкала по дням) и правка. "
                   "Ключи: атрибуты schedule (и mode) устройства; сохраняет shared-атрибут set_schedule.",
    "descriptor": {
        "type": "latest",
        "sizeX": 10,
        "sizeY": 6.5,
        "resources": [],
        "templateHtml": "",
        "templateCss": (SRC / "style.css").read_text(encoding="utf-8"),
        "controllerScript": (SRC / "controller.js").read_text(encoding="utf-8"),
        "defaultConfig": json.dumps(default_config, ensure_ascii=False),
    },
    "tags": ["opentherm", "schedule", "thermostat"],
}

bundle = {
    "widgetsBundle": {
        "alias": "opentherm",
        "title": "OpenTherm",
        "image": None,
        "description": "Виджеты платы OpenTherm",
        "order": None,
    },
    "widgetTypes": [widget],
}

# Widgets library -> Widgets bundles -> + -> Import widgets bundle (preferred),
# or Widgets library -> Widgets -> + -> Import widget. Not a dashboard import.
for name, data in (("opentherm_widgets_bundle.json", bundle), ("opentherm_schedule_widget.json", widget)):
    out = HERE / name
    out.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(out)
