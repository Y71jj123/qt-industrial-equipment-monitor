#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MQTT 数据发布模拟器 —— 用于测试项目的 MQTT 接入。

纯标准库实现最小 MQTT 3.1.1 客户端（CONNECT + PUBLISH），**不需要装 paho-mqtt**。

用法
----
    # 公共测试 broker（需联网，任何 topic 都能发）
    python mqtt_publisher_sim.py --host broker.emqx.io --topic factory/line1

    # 本地 mosquitto / EMQX（如果装了）
    python mqtt_publisher_sim.py --host 127.0.0.1 --topic factory/line1

然后在项目的「添加设备」里填：

    协议      MQTT
    地址      broker.emqx.io    （或 127.0.0.1）
    端口      1883
    订阅主题  factory/line1/#

发布格式（本脚本两种都能演示）
------------------------------
    --mode json     一个主题发全部：factory/line1/data 载荷 {"temperature":42.53,...}
    --mode single   每点位一个主题：factory/line1/temperature 载荷 42.53

项目的解析规则：**主题末级 或 JSON 字段名 = 点位 ID**，所以两种都能直接对上。
"""

from __future__ import annotations

import argparse
import json
import random
import socket
import struct
import sys
import time

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# 点位 → (基准值, 下限, 上限)
TAGS = {
    "temperature": (42.0, 25.0, 68.0),
    "pressure": (55.0, 30.0, 85.0),
    "speed": (58.0, 30.0, 88.0),
    "vibration": (30.0, 10.0, 60.0),
}


def encode_length(value: int) -> bytes:
    """MQTT 剩余长度（7 位一组，最高位做延续标志）。"""
    out = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value > 0:
            byte |= 0x80
        out.append(byte)
        if value == 0:
            break
    return bytes(out)


def encode_string(text: str) -> bytes:
    data = text.encode("utf-8")
    return struct.pack(">H", len(data)) + data


def build_connect(client_id: str, keepalive: int = 60) -> bytes:
    variable = encode_string("MQTT") + bytes([0x04, 0x02]) + struct.pack(">H", keepalive)
    payload = encode_string(client_id)
    return bytes([0x10]) + encode_length(len(variable) + len(payload)) + variable + payload


def build_publish(topic: str, payload: str) -> bytes:
    variable = encode_string(topic)
    data = payload.encode("utf-8")
    return bytes([0x30]) + encode_length(len(variable) + len(data)) + variable + data


def main() -> None:
    parser = argparse.ArgumentParser(description="MQTT 数据发布模拟器")
    parser.add_argument("--host", default="broker.emqx.io", help="broker 地址")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--topic", default="factory/line1", help="主题前缀")
    parser.add_argument("--mode", choices=["json", "single"], default="json")
    parser.add_argument("--interval", type=float, default=1.0, help="发布周期秒")
    parser.add_argument("--client-id", default="dsh-sim-publisher")
    args = parser.parse_args()

    print(f"正在连接 broker {args.host}:{args.port} …")
    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as error:
        print(f"连接失败：{error}")
        return

    sock.sendall(build_connect(args.client_id))

    try:
        connack = sock.recv(4)
    except OSError as error:
        print(f"读取 CONNACK 失败：{error}")
        sock.close()
        return

    if len(connack) < 4 or connack[0] != 0x20:
        print("未收到 CONNACK，连接失败")
        sock.close()
        return
    if connack[3] != 0:
        print(f"broker 拒绝连接，返回码 {connack[3]}")
        sock.close()
        return

    print(f"已连接。开始向 {args.topic} 发布数据（Ctrl+C 退出）")

    values = {name: spec[0] for name, spec in TAGS.items()}

    try:
        while True:
            for name, (_, low, high) in TAGS.items():
                baseline = TAGS[name][0]
                current = values[name] + (baseline - values[name]) * 0.10 + random.uniform(-2.0, 2.0)
                values[name] = max(low, min(high, current))

            if args.mode == "json":
                payload = json.dumps({k: round(v, 2) for k, v in values.items()})
                sock.sendall(build_publish(f"{args.topic}/data", payload))
                print(f"  → {args.topic}/data  {payload}")
            else:
                for name, value in values.items():
                    sock.sendall(build_publish(f"{args.topic}/{name}", f"{value:.2f}"))
                print("  → " + ", ".join(f"{k}={v:.2f}" for k, v in values.items()))

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n已退出")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
