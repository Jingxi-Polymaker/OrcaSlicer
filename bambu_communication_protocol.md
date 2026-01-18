# Bambu Printer Communication Protocol (LAN, High-Level)

## Scope
This document summarizes the Bambu Labs printer protocol as observed from OrcaSlicer.
It is based on the captured logs in `printer_comm_20260118_123710.md` and
`printer_comm_20260118_183556_with_bind_unbind.md` plus the parsing logic in
`MachineObject::parse_json()` and AMS parsing helpers. It does not cover OrcaCloud behavior.

## Transport and framing
- Payloads are JSON objects exchanged over LAN (log shows `LAN`; implementation suggests MQTT).
- Each message uses a single top-level namespace: `pushing`, `info`, `print`, or `system`.
- Each namespace carries a `command` string and typically a `sequence_id` (string).
- Responses usually echo `sequence_id` and add `result`, `reason`, `errno`, and/or `is_from_mqtt`.
- Status updates arrive asynchronously via `print.push_status` (full or diff updates).

## Observed command catalog (from log)
| Command | Direction | Purpose |
| --- | --- | --- |
| `pushing.pushall` | Host -> Printer | Request full status push |
| `info.get_version` | Host <-> Printer | Query firmware module versions |
| `print.push_status` | Printer -> Host | Periodic status snapshot |
| `print.set_nozzle_temp` | Host <-> Printer | Set nozzle target temperature |
| `print.set_bed_temp` | Host <-> Printer | Set bed target temperature |
| `print.xyz_ctrl` | Host <-> Printer | Axis jog control |
| `print.back_to_center` | Host <-> Printer | Move toolhead to center |
| `print.extrusion_cali_get` | Host <-> Printer | Query extrusion calibration data |
| `print.extrusion_cali_sel` | Host <-> Printer | Select extrusion calibration slot |
| `print.ams_filament_setting` | Host <-> Printer | Update AMS tray metadata |
| `print.ams_user_setting` | Host <-> Printer | Update AMS user settings (startup/tray read, calibration) |
| `print.print_option` | Host <-> Printer | Update print options (auto switch filament) |
| `print.select_extruder` | Host <-> Printer | Select active extruder index |
| `print.set_fan` | Host <-> Printer | Set fan speed by index |
| `print.set_airduct` | Host <-> Printer | Set airduct mode/submode |
| `system.ledctrl` | Host <-> Printer | Control chamber lights |
| `print.gcode_line` | Printer -> Host | G-code line ack/result |

## Major features and protocols

### Status streaming and telemetry
- `pushing.pushall` requests a full snapshot; printer replies with `print.push_status`.
- `print.push_status` is the primary telemetry stream and includes:
  - Print state, job metadata, and progress fields (e.g., `job_id`, `state`, `percent`).
  - `device` object with nozzle/extruder/bed/airduct/laser details.
  - `ams` (automatic material system) and `vir_slot` (virtual/external spool) data.
  - `xcam` and `xcam_status` for AI camera features.
  - `hms` health/diagnostics list (empty in this log).
- Code supports diff updates (`print.msg == 1`) with base restoration; log shows full snapshots.

### Filament and AMS management
- AMS data is carried inside `print.push_status` under `print.ams` and `print.vir_slot`.
- Tray metadata updates use `print.ams_filament_setting`, returning `result: success` on ack.
- `print.ams_user_setting` appears in the new log and toggles AMS read/calibration flags.
- `print.print_option` appears in the new log with `auto_switch_filament` updates.
- Additional AMS commands present in code but not observed in this log:
  - `print.ams_change_filament`, `print.ams_get_rfid`, `print.ams_control`,
    `print.auto_stop_ams_dry`.
- Code also uses `print.print_option` with `air_print_detect` fields
  for AMS auto-refill and air-print detection (not observed in log).

### Motion control
- `print.xyz_ctrl` jogs axes using `axis`, `dir`, and `mode`; response includes `result`/`errno`.
- `print.back_to_center` recenters the toolhead; response includes `result`/`errno`.
- `print.select_extruder` switches active extruder by `extruder_index` and returns `result`/`errno`.

### Thermal control
- `print.set_nozzle_temp` sets nozzle target per `extruder_index`.
- `print.set_bed_temp` sets bed target.
- Code also supports chamber temperature via `print.set_ctt` (not observed in log).

### Lighting
- `system.ledctrl` controls chamber lights via `led_node`, `led_mode`, and timing fields.

### Fan and airduct control
- `print.set_fan` sets fan speed by `fan_index` and `speed`.
- `print.set_airduct` updates airduct `modeId`/`submode` and triggers status updates.

### Calibration
- `print.extrusion_cali_get` and `print.extrusion_cali_sel` appear in the log.
- Code also handles `print.extrusion_cali`, `print.flowrate_cali`,
  `print.extrusion_cali_set`, and `print.flowrate_get_result` (not observed).

### Camera and AI detection
- `print.push_status` includes `xcam` fields (e.g., `spaghetti_detector`, `printing_monitor`).
- Code supports `print.xcam_control_set` and `camera.ipcam_*` commands (not observed).

### Print/job lifecycle
- `print.gcode_line` responses acknowledge line-level operations.
- Code handles `print.project_prepare` and `print.project_file` commands (not observed).

### Access/bind and settings
- Code in this repo constructs `system.get_access_code`, `system.set_door_stat`, and
  `system.print_cache_set` messages.
- Bind/connect/disconnect flows are handled by the networking plugin and mockup server, but the
  raw `system.bind`/`system.connect`/`system.disconnect` payloads are not visible in the logs here.

## Verification status
- Observed in log: `pushing.pushall`, `info.get_version`, `print.push_status`,
  `print.set_nozzle_temp`, `print.set_bed_temp`, `print.xyz_ctrl`, `print.back_to_center`,
  `print.extrusion_cali_get`, `print.extrusion_cali_sel`, `print.ams_filament_setting`,
  `print.ams_user_setting`, `print.print_option`, `print.select_extruder`, `print.set_fan`,
  `print.set_airduct`, `system.ledctrl`, `print.gcode_line`.
- To verify (code-supported, not in log): AMS control commands, chamber temp (`set_ctt`),
  `print.print_option` `air_print_detect`, xcam control, camera `ipcam_*`, project file commands,
  and plugin-managed bind/connect operations (`system.bind`, `system.connect`, `system.disconnect`).

## Sources
- Log: `printer_comm_20260118_123710.md`
- Log: `printer_comm_20260118_183556_with_bind_unbind.md`
- Parsing: `src/slic3r/GUI/DeviceManager.cpp`
- AMS parsing: `src/slic3r/GUI/DeviceCore/DevFilaSystem.cpp`
- Sync data flow: `src/slic3r/GUI/Plater.cpp`
