# The live dashboard: feeding it from Home Assistant

The panel's first dashboard, **Live**, is drawn on the device: the clock, the next alarm, tonight's timeline, and seven tiles. The A/C tile, the alarm, the sleep timer and sunrise come from the panel itself. The other tiles read Home Assistant over the same MQTT connection the panel already uses for discovery. No Home Assistant token is needed.

Each tile has a **source**, set in the web app under Device › Live dashboard. A source can be:

- **An entity id,** such as `sensor.bedroom_temperature`. The panel reads it through MQTT Statestream at `homeassistant_states/sensor/bedroom_temperature/state`. The Statestream base topic is set in the same card.
- **Any MQTT topic,** written with a `/`.
- **Empty,** in which case the panel listens on `bedside/live/<tile>`. An automation can publish there. The hourly rain chart and the next calendar event have to arrive this way, because they aren't single entity states.

| Tile | Key | Expects |
|---|---|---|
| Outside | `outside_temp`, `outside_cond`, `outside_high`, `outside_low` | numbers, and a condition such as `rainy` or `partlycloudy` |
| Chance of rain | `rain_hourly` | up to 8 numbers, `70,60,45,30,20,10,5,5` (now, +1 h, ...) |
| Bedroom | `bedroom_temp`, `bedroom_humidity` | numbers; the panel draws the 12-hour trend itself |
| Washing | `washer` | any text; `done`, `finished` or `complete` turn the tile black |
| Security | `front_door`, `back_door`, `garage` | `locked`, `closed`, `off` or `shut` count as shut; anything else turns the tile black |
| Bins | `bins`, `bins_when` | text, such as `Recycling` and `Out tonight` |
| Next event | `next_event` | `HH:MM Title`, such as `09:00 Dentist` |

## Statestream (single entities)

```yaml
mqtt_statestream:
  base_topic: homeassistant_states
  include:
    entities:
      - sensor.bedroom_temperature
      - sensor.bedroom_humidity
      - lock.front_door
      - lock.back_door
      - cover.garage_door
      - sensor.washing_machine_status
```

Then put those entity ids in the matching tiles' sources.

## Automations (forecast and calendar)

```yaml
automation:
  - alias: Bedside panel, forecast
    triggers:
      - trigger: time_pattern
        minutes: "/15"
    actions:
      - action: weather.get_forecasts
        target: { entity_id: weather.home }
        data: { type: hourly }
        response_variable: hourly
      - action: weather.get_forecasts
        target: { entity_id: weather.home }
        data: { type: daily }
        response_variable: daily
      - action: mqtt.publish
        data:
          topic: bedside/live/rain_hourly
          retain: true
          payload: "{{ hourly['weather.home'].forecast[:8] | map(attribute='precipitation_probability') | join(',') }}"
      - action: mqtt.publish
        data: { topic: bedside/live/outside_high, retain: true, payload: "{{ daily['weather.home'].forecast[0].temperature | round(0) }}" }
      - action: mqtt.publish
        data: { topic: bedside/live/outside_low, retain: true, payload: "{{ daily['weather.home'].forecast[0].templow | round(0) }}" }
      - action: mqtt.publish
        data: { topic: bedside/live/outside_temp, retain: true, payload: "{{ state_attr('weather.home', 'temperature') }}" }
      - action: mqtt.publish
        data: { topic: bedside/live/outside_cond, retain: true, payload: "{{ states('weather.home') }}" }

  - alias: Bedside panel, next event
    triggers:
      - trigger: time_pattern
        minutes: "/10"
    actions:
      - action: calendar.get_events
        target: { entity_id: calendar.home }
        data: { duration: { hours: 36 } }
        response_variable: ev
      - action: mqtt.publish
        data:
          topic: bedside/live/next_event
          retain: true
          payload: >-
            {% set e = ev['calendar.home'].events | first %}
            {{ (as_datetime(e.start) | as_local).strftime('%H:%M') ~ ' ' ~ e.summary if e else '' }}
```

Use `retain: true` so the panel has the values as soon as it connects.
