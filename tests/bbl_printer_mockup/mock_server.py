#!/usr/bin/env python3
import argparse
import asyncio
import json
import time
from copy import deepcopy


DEFAULT_ACCESS_CODE = "000000"


def _now_ms() -> int:
    return int(time.time() * 1000)


def _is_bbl_tag(tag_uid: str) -> bool:
    if not tag_uid:
        return False
    return any(ch != "0" for ch in tag_uid)


class PrinterState:
    def __init__(self) -> None:
        self.sequence_id = 20000
        self.ams_units, self.vir_slots = self._default_filament_state()

    def next_sequence_id(self) -> str:
        self.sequence_id += 1
        return str(self.sequence_id)

    def _default_filament_state(self):
        ams_units = [
            {
                "id": "0",
                "info": "1103",
                "humidity": "2",
                "humidity_raw": "28",
                "temp": "34.0",
                "dry_time": 0,
                "tray": [
                    {
                        "id": "0",
                        "tray_info_idx": "GFB99",
                        "tray_type": "ABS",
                        "tray_color": "0D6284FF",
                        "nozzle_temp_min": "240",
                        "nozzle_temp_max": "280",
                        "tag_uid": "0000000000000000",
                        "ctype": 0,
                        "cols": ["0D6284FF"],
                        "remain": -1,
                        "state": 11,
                        "tray_diameter": "1.75",
                        "tray_uuid": "00000000000000000000000000000000",
                    },
                    {"id": "1", "state": 0},
                    {
                        "id": "2",
                        "tray_info_idx": "GFB98",
                        "tray_type": "ASA",
                        "tray_color": "161616FF",
                        "nozzle_temp_min": "240",
                        "nozzle_temp_max": "280",
                        "tag_uid": "0000000000000000",
                        "ctype": 0,
                        "cols": ["161616FF"],
                        "remain": -1,
                        "state": 11,
                        "tray_diameter": "1.75",
                        "tray_uuid": "00000000000000000000000000000000",
                    },
                    {"id": "3", "state": 0},
                ],
            }
        ]
        vir_slots = [
            {
                "id": "254",
                "tray_info_idx": "",
                "tray_type": "",
                "tray_color": "00000000",
                "nozzle_temp_min": "0",
                "nozzle_temp_max": "0",
                "tag_uid": "0000000000000000",
                "ctype": 0,
                "cols": ["00000000"],
                "remain": 0,
                "tray_diameter": "1.75",
                "tray_uuid": "00000000000000000000000000000000",
            }
        ]
        return ams_units, vir_slots

    def _tray_present(self, tray: dict) -> bool:
        if tray.get("tray_info_idx"):
            return True
        state = tray.get("state", 0)
        return state not in (0, None)

    def _calc_ams_exist_bits(self) -> str:
        bits = 0
        for ams in self.ams_units:
            try:
                ams_id = int(ams.get("id", "-1"))
            except ValueError:
                continue
            if 0 <= ams_id < 16:
                bits |= 1 << ams_id
        return format(bits, "x")

    def _calc_tray_bits(self):
        exist_bits = 0
        bbl_bits = 0
        for ams in self.ams_units:
            try:
                ams_id = int(ams.get("id", "-1"))
            except ValueError:
                continue
            if ams_id < 0 or ams_id >= 16:
                continue
            for tray in ams.get("tray", []):
                try:
                    tray_id = int(tray.get("id", "-1"))
                except ValueError:
                    continue
                if tray_id < 0 or tray_id >= 4:
                    continue
                bit = 1 << (ams_id * 4 + tray_id)
                if self._tray_present(tray):
                    exist_bits |= bit
                if _is_bbl_tag(tray.get("tag_uid", "")):
                    bbl_bits |= bit
        return format(exist_bits, "x"), format(bbl_bits, "x")

    def build_push_status(self) -> dict:
        ams_exist_bits = self._calc_ams_exist_bits()
        tray_exist_bits, tray_is_bbl_bits = self._calc_tray_bits()
        return {
            "t_utc": _now_ms(),
            "print": {
                "command": "push_status",
                "sequence_id": self.next_sequence_id(),
                "ams": {
                    "ams": deepcopy(self.ams_units),
                    "ams_exist_bits": ams_exist_bits,
                    "ams_exist_bits_raw": ams_exist_bits,
                    "tray_exist_bits": tray_exist_bits,
                    "tray_is_bbl_bits": tray_is_bbl_bits,
                    "tray_read_done_bits": tray_exist_bits,
                    "tray_reading_bits": "0",
                    "tray_now": "255",
                    "tray_pre": "255",
                    "tray_tar": "255",
                    "cali_id": 255,
                    "cali_stat": 0,
                    "insert_flag": True,
                    "power_on_flag": False,
                    "version": 157118,
                    "unbind_ams_stat": 0,
                },
                "vir_slot": deepcopy(self.vir_slots),
            },
        }

    def apply_ams_filament_setting(self, payload: dict) -> dict:
        ams_id = payload.get("ams_id")
        slot_id = payload.get("slot_id")
        tray_id = payload.get("tray_id")

        def _update_tray(tray: dict) -> None:
            if "tray_info_idx" in payload:
                tray["tray_info_idx"] = payload["tray_info_idx"]
            if "tray_type" in payload:
                tray["tray_type"] = payload["tray_type"]
            if "tray_color" in payload:
                tray["tray_color"] = payload["tray_color"]
                tray["cols"] = [payload["tray_color"]]
            if "nozzle_temp_min" in payload:
                tray["nozzle_temp_min"] = str(payload["nozzle_temp_min"])
            if "nozzle_temp_max" in payload:
                tray["nozzle_temp_max"] = str(payload["nozzle_temp_max"])
            if "setting_id" in payload:
                tray["setting_id"] = payload["setting_id"]
            tray.setdefault("tag_uid", "0000000000000000")
            tray.setdefault("ctype", 0)
            tray.setdefault("remain", -1)
            tray["state"] = tray.get("state", 11) or 11

        if ams_id in (254, 255):
            target_id = str(ams_id if tray_id is None else tray_id)
            for tray in self.vir_slots:
                if tray.get("id") == target_id:
                    _update_tray(tray)
                    return tray
            new_tray = {"id": target_id}
            _update_tray(new_tray)
            self.vir_slots.append(new_tray)
            return new_tray

        target_ams = None
        for ams in self.ams_units:
            if ams.get("id") == str(ams_id):
                target_ams = ams
                break
        if target_ams is None:
            target_ams = {"id": str(ams_id), "info": "1103", "tray": []}
            self.ams_units.append(target_ams)

        target_tray_id = str(slot_id if slot_id is not None else tray_id)
        for tray in target_ams.get("tray", []):
            if tray.get("id") == target_tray_id:
                _update_tray(tray)
                return tray

        new_tray = {"id": target_tray_id}
        _update_tray(new_tray)
        target_ams.setdefault("tray", []).append(new_tray)
        return new_tray


class ClientSession:
    def __init__(self, state: PrinterState, writer: asyncio.StreamWriter, push_interval: float, access_code: str) -> None:
        self.state = state
        self.writer = writer
        self.push_interval = push_interval
        self.access_code = access_code
        self.bound = False
        self.connected = False
        self.push_task = None

    async def send_json(self, payload: dict) -> None:
        data = json.dumps(payload, separators=(",", ":")) + "\n"
        self.writer.write(data.encode("utf-8"))
        await self.writer.drain()

    async def start_push(self) -> None:
        if self.push_task is not None:
            return

        async def _loop():
            while True:
                await asyncio.sleep(self.push_interval)
                await self.send_json(self.state.build_push_status())

        self.push_task = asyncio.create_task(_loop())

    async def stop_push(self) -> None:
        if self.push_task is None:
            return
        self.push_task.cancel()
        try:
            await self.push_task
        except asyncio.CancelledError:
            pass
        self.push_task = None

    async def handle_message(self, msg: dict, auto_push: bool) -> None:
        if "system" in msg:
            await self._handle_system(msg["system"], auto_push)
            return
        if "pushing" in msg:
            await self._handle_pushing(msg["pushing"], auto_push)
            return
        if "info" in msg:
            await self._handle_info(msg["info"])
            return
        if "print" in msg:
            await self._handle_print(msg["print"])
            return

        await self.send_json({"error": "unknown_namespace"})

    async def _handle_system(self, payload: dict, auto_push: bool) -> None:
        cmd = payload.get("command")
        seq = payload.get("sequence_id", self.state.next_sequence_id())

        if cmd == "get_access_code":
            await self.send_json(
                {"system": {"command": cmd, "sequence_id": seq, "access_code": self.access_code}}
            )
            return

        if cmd == "bind":
            req_code = payload.get("access_code", "")
            ok = (not req_code) or (req_code == self.access_code)
            self.bound = ok
            await self.send_json(
                {
                    "system": {
                        "command": cmd,
                        "sequence_id": seq,
                        "result": "success" if ok else "fail",
                        "reason": "" if ok else "access_code_mismatch",
                    }
                }
            )
            return

        if cmd == "connect":
            self.connected = True
            if auto_push:
                await self.start_push()
            await self.send_json({"system": {"command": cmd, "sequence_id": seq, "result": "success"}})
            return

        if cmd == "disconnect":
            self.connected = False
            await self.stop_push()
            await self.send_json({"system": {"command": cmd, "sequence_id": seq, "result": "success"}})
            self.writer.close()
            return

        await self.send_json({"system": {"command": cmd, "sequence_id": seq, "result": "unknown_command"}})

    async def _handle_pushing(self, payload: dict, auto_push: bool) -> None:
        cmd = payload.get("command")
        seq = payload.get("sequence_id", self.state.next_sequence_id())

        if cmd == "pushall":
            await self.send_json(self.state.build_push_status())
            return

        if cmd == "start":
            self.connected = True
            if auto_push:
                await self.start_push()
            await self.send_json({"pushing": {"command": cmd, "sequence_id": seq, "result": "success"}})
            return

        if cmd == "stop":
            await self.stop_push()
            self.connected = False
            await self.send_json({"pushing": {"command": cmd, "sequence_id": seq, "result": "success"}})
            return

        await self.send_json({"pushing": {"command": cmd, "sequence_id": seq, "result": "unknown_command"}})

    async def _handle_info(self, payload: dict) -> None:
        cmd = payload.get("command")
        seq = payload.get("sequence_id", self.state.next_sequence_id())
        if cmd == "get_version":
            await self.send_json(
                {
                    "info": {
                        "command": cmd,
                        "sequence_id": seq,
                        "module": [
                            {
                                "name": "ota",
                                "product_name": "Mock Bambu Printer",
                                "sn": "MOCK000000000000",
                                "sw_ver": "01.02.02.00",
                                "hw_ver": "N/A",
                                "flag": 3,
                                "visible": True,
                            },
                            {
                                "name": "n3f/0",
                                "product_name": "Mock AMS",
                                "sn": "MOCKAMS00000000",
                                "sw_ver": "03.00.21.29",
                                "hw_ver": "N3F05",
                                "flag": 0,
                                "visible": True,
                            },
                        ],
                    }
                }
            )
            return

        await self.send_json({"info": {"command": cmd, "sequence_id": seq, "result": "unknown_command"}})

    async def _handle_print(self, payload: dict) -> None:
        cmd = payload.get("command")
        seq = payload.get("sequence_id", self.state.next_sequence_id())
        if cmd == "ams_filament_setting":
            self.state.apply_ams_filament_setting(payload)
            response = deepcopy(payload)
            response["sequence_id"] = seq
            response["result"] = "success"
            response["is_from_mqtt"] = True
            await self.send_json({"print": response})
            await self.send_json(self.state.build_push_status())
            return

        await self.send_json(
            {
                "print": {
                    "command": cmd,
                    "sequence_id": seq,
                    "result": "success",
                    "errno": 0,
                    "reason": "",
                }
            }
        )


async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, state: PrinterState, access_code: str, push_interval: float, auto_push: bool) -> None:
    session = ClientSession(state, writer, push_interval, access_code)
    while True:
        data = await reader.readline()
        if not data:
            break
        line = data.decode("utf-8").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            await session.send_json({"error": "invalid_json"})
            continue
        await session.handle_message(msg, auto_push)

    await session.stop_push()
    writer.close()


async def run_server(host: str, port: int, access_code: str, push_interval: float, auto_push: bool) -> None:
    state = PrinterState()
    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, state, access_code, push_interval, auto_push),
        host,
        port,
    )
    addrs = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print(f"Mock BBL printer server listening on {addrs}")
    async with server:
        await server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="Mock Bambu printer server for sync_ams_list testing.")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=19000, help="Bind port (default: 19000)")
    parser.add_argument("--access-code", default=DEFAULT_ACCESS_CODE, help="Access code for bind/get_access_code")
    parser.add_argument("--push-interval", type=float, default=2.0, help="Interval for push_status when streaming")
    parser.add_argument("--auto-push", action="store_true", help="Auto-stream push_status after connect/start")
    args = parser.parse_args()

    try:
        asyncio.run(run_server(args.host, args.port, args.access_code, args.push_interval, args.auto_push))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
