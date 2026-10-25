#!/usr/bin/env python3
"""按共享 Schema 校验真实 mock socket 抓包；响应按原操作配对。"""
import json
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "contracts"))
from validate import load_validators  # noqa: E402


def check_trace(trace):
    validator = load_validators()["firmware-gateway.schema.json"]
    pending, identifiers = {}, set()
    counts = {"device": 0, "server": 0}
    for entry in trace:
        direction, packet = entry["direction"], entry["packet"]
        if direction not in counts:
            raise ValueError("未知抓包方向")
        definition = "FirmwareToGateway" if direction == "device" else "GatewayToFirmware"
        validator.evolve(schema=validator.schema["$defs"][definition]).validate(packet)
        if direction == "device":
            if packet["id"] in identifiers:
                raise ValueError("设备网络包 ID 被重复使用")
            identifiers.add(packet["id"])
            pending[packet["id"]] = packet["type"]
        elif packet["type"] == "response":
            operation = pending.pop(packet["reply_to"], None)
            if operation is None:
                raise ValueError("响应没有对应的在途操作")
            validator.evolve(schema=validator.schema["$defs"][operation + ".response"]).validate(packet)
        counts[direction] += 1
    if not all(counts.values()):
        raise ValueError("缺少双向真实网络报文")
    # Confirm response may deliberately be lost; pending entries are not success.
    return {**counts, "unanswered": len(pending)}


if __name__ == "__main__":
    print(json.dumps(check_trace(json.loads(Path(sys.argv[1]).read_text())), ensure_ascii=False))
