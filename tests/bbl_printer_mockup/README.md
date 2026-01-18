# BBL Printer Mockup Server

This mock server simulates a subset of the Bambu printer LAN protocol for sync_ams_list testing.
It is a simple TCP server that exchanges line-delimited JSON payloads and is not an MQTT broker.

## Run
```sh
python3 tests/bbl_printer_mockup/mock_server.py --host 127.0.0.1 --port 19000 --auto-push
```

## Supported commands
- `system.get_access_code`
- `system.bind`
- `system.connect`
- `system.disconnect`
- `pushing.pushall`
- `pushing.start` / `pushing.stop`
- `info.get_version`
- `print.ams_filament_setting`

## Example session
Open a TCP client and send JSON lines:
```sh
nc 127.0.0.1 19000
```

```json
{"system":{"command":"get_access_code","sequence_id":"1"}}
{"system":{"command":"bind","sequence_id":"2","access_code":"000000"}}
{"system":{"command":"connect","sequence_id":"3"}}
{"pushing":{"command":"pushall","sequence_id":"4","version":1,"push_target":1}}
{"print":{"command":"ams_filament_setting","sequence_id":"5","ams_id":0,"slot_id":0,"tray_id":0,"tray_info_idx":"GFB60","setting_id":"GFSB60_06","tray_type":"ABS","tray_color":"0D6284FF","nozzle_temp_min":240,"nozzle_temp_max":280}}
```

Notes:
- `--auto-push` enables periodic `print.push_status` after `system.connect` or `pushing.start`.
- Without `--auto-push`, the server only emits `print.push_status` on `pushing.pushall` or after a tray update.
