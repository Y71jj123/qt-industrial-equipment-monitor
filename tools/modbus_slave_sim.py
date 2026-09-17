#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地 Modbus TCP 从站模拟器 —— 不接真 PLC 也能跑通项目的 Modbus 通路。

纯标准库实现（socket + struct + threading），**不需要安装任何依赖**。

用法
----
    python modbus_slave_sim.py                  # 监听 127.0.0.1:5020
    python modbus_slave_sim.py --port 502       # 换端口（502 在 Windows 上需管理员权限）
    python modbus_slave_sim.py --port 5021      # 想同时开多个"设备"就换端口

然后在项目的「添加设备」里填：

    协议      Modbus TCP
    地址      127.0.0.1
    端口      5020
    从站地址  1
    采集周期  1000 ms
    点位表    把 5 行的「缩放」列全改成 **0.01**

寄存器映射（保持寄存器 / 功能码 03）
------------------------------------
    0  →  温度        ℃        基准 42
    1  →  压力        kPa      基准 55
    2  →  转速        r/min    基准 58
    3  →  运行状态    0 / 1
    4  →  振动        mm/s     基准 30

寄存器里存的是 **工程值 × 100** 的整数（42.53℃ 存 4253），所以点位「缩放」填 0.01。
数值每秒按"均值回归 + 噪声"更新一次，会自然地在阈值附近波动，方便观察告警。
"""

from __future__ import annotations

import argparse
import random
import socket
import struct
import sys
import threading
import time

try:  # Windows 控制台中文输出
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

SCALE = 100
REGISTER_COUNT = 50

# 地址 → (名称, 基准值, 下限, 上限)
ANALOG = {
    0: ("temperature", 42.0, 25.0, 68.0),
    1: ("pressure", 55.0, 30.0, 85.0),
    2: ("speed", 58.0, 30.0, 88.0),
    4: ("vibration", 30.0, 10.0, 60.0),
}
RUNNING_ADDR = 3


def recv_exact(conn: socket.socket, count: int):
    """读满 count 字节；连接关闭返回 None。"""
    buf = b""
    while len(buf) < count:
        chunk = conn.recv(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class Simulator:
    """寄存器组 + 数据更新 + PDU 处理。"""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.holding = [0] * REGISTER_COUNT       # 保持寄存器（03 读 / 06 写）
        self.input_regs = [0] * REGISTER_COUNT    # 输入寄存器（04 读）
        self.coils = [0] * REGISTER_COUNT         # 线圈（01 读 / 05 写）
        self.discrete = [0] * REGISTER_COUNT      # 离散输入
        self.base = {addr: spec[1] for addr, spec in ANALOG.items()}

        for addr, (_, base, _, _) in ANALOG.items():
            self.holding[addr] = int(round(base * SCALE))
            self.input_regs[addr] = self.holding[addr]

        self.holding[RUNNING_ADDR] = SCALE
        self.input_regs[RUNNING_ADDR] = SCALE
        self.coils[RUNNING_ADDR] = 1
        self.discrete[RUNNING_ADDR] = 1

    # ---------------- 数据更新 ----------------

    def run_updater(self, interval: float = 1.0) -> None:
        while True:
            time.sleep(interval)
            with self.lock:
                for addr, (_, _, low, high) in ANALOG.items():
                    current = self.holding[addr] / SCALE
                    baseline = self.base[addr]
                    # 均值回归 + 小幅扰动：不会随机游走到边界卡住
                    current += (baseline - current) * 0.10 + random.uniform(-2.0, 2.0)
                    current = max(low, min(high, current))
                    raw = int(round(current * SCALE))
                    self.holding[addr] = raw
                    self.input_regs[addr] = raw

                running = 1 if random.random() < 0.85 else 0
                self.holding[RUNNING_ADDR] = running * SCALE
                self.input_regs[RUNNING_ADDR] = running * SCALE
                self.coils[RUNNING_ADDR] = running
                self.discrete[RUNNING_ADDR] = running

    # ---------------- 协议处理 ----------------

    def handle_pdu(self, pdu: bytes) -> bytes:
        if not pdu:
            return b""

        function = pdu[0]
        try:
            if function in (0x03, 0x04):  # 读保持 / 输入寄存器
                start, count = struct.unpack(">HH", pdu[1:5])
                with self.lock:
                    regs = self.holding if function == 0x03 else self.input_regs
                    values = regs[start:start + count]
                if count == 0 or count > 125 or len(values) != count:
                    return bytes([function | 0x80, 0x02])  # 非法数据地址
                payload = b"".join(struct.pack(">H", v) for v in values)
                return bytes([function, len(payload)]) + payload

            if function == 0x06:  # 写单寄存器
                address, value = struct.unpack(">HH", pdu[1:5])
                with self.lock:
                    if address < REGISTER_COUNT:
                        self.holding[address] = value
                print(f"    ← 收到写入：寄存器 {address} = {value}")
                return pdu[:5]

            if function == 0x01:  # 读线圈
                start, count = struct.unpack(">HH", pdu[1:5])
                with self.lock:
                    bits = self.coils[start:start + count]
                if count == 0 or len(bits) != count:
                    return bytes([function | 0x80, 0x02])
                byte_count = (count + 7) // 8
                out = bytearray(byte_count)
                for index, bit in enumerate(bits):
                    if bit:
                        out[index // 8] |= 1 << (index % 8)
                return bytes([function, byte_count]) + bytes(out)

            if function == 0x05:  # 写单线圈
                address, value = struct.unpack(">HH", pdu[1:5])
                with self.lock:
                    if address < REGISTER_COUNT:
                        self.coils[address] = 1 if value == 0xFF00 else 0
                print(f"    ← 收到写线圈：线圈 {address} = {self.coils[address]}")
                return pdu[:5]

        except struct.error:
            return bytes([function | 0x80, 0x03])  # 非法数据值

        return bytes([function | 0x80, 0x01])  # 非法功能码


def serve_client(conn: socket.socket, address, simulator: Simulator) -> None:
    print(f"[+] 客户端已连接：{address[0]}:{address[1]}")
    with conn:
        while True:
            header = recv_exact(conn, 7)  # MBAP 头
            if header is None:
                break
            transaction_id, protocol_id, length, unit_id = struct.unpack(">HHHB", header)

            pdu = recv_exact(conn, length - 1)
            if pdu is None:
                break

            response = simulator.handle_pdu(pdu)
            frame = struct.pack(">HHHB", transaction_id, protocol_id, len(response) + 1, unit_id)
            conn.sendall(frame + response)
    print(f"[-] 客户端断开：{address[0]}:{address[1]}")


def main() -> None:
    parser = argparse.ArgumentParser(description="本地 Modbus TCP 从站模拟器")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址（默认 127.0.0.1）")
    parser.add_argument("--port", type=int, default=5020, help="监听端口（默认 5020）")
    parser.add_argument("--slave", type=int, default=1, help="从站地址（默认 1）")
    parser.add_argument("--interval", type=float, default=1.0, help="数据更新周期秒（默认 1.0）")
    args = parser.parse_args()

    simulator = Simulator()
    threading.Thread(target=simulator.run_updater, args=(args.interval,), daemon=True).start()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(5)

    print("=" * 66)
    print("  Modbus TCP 从站模拟器已启动")
    print(f"  监听        {args.host}:{args.port}   从站地址 {args.slave}")
    print("  寄存器      0=温度  1=压力  2=转速  3=运行  4=振动")
    print("  取值        寄存器 = 工程值 × 100（项目里点位「缩放」填 0.01）")
    print("  退出        Ctrl + C")
    print("=" * 66)

    try:
        while True:
            conn, address = server.accept()
            threading.Thread(target=serve_client, args=(conn, address, simulator), daemon=True).start()
    except KeyboardInterrupt:
        print("\n已退出")
    finally:
        server.close()


if __name__ == "__main__":
    main()
