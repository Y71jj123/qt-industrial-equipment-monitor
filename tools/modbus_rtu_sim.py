#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Modbus RTU 从站模拟器（仅用于本地开发验证，不是生产代码）。

监听一个串口，响应最基础的读请求：
    - FC 0x01 读线圈
    - FC 0x03 读保持寄存器
    - FC 0x04 读输入寄存器
写请求（FC 0x05 / 0x06）暂不做响应，本模拟器只为验证"采集链路能跑通"。

寄存器里的值 = 寄存器地址本身（方便在界面上核对"点位 X 读回来就是 X"）。
例如：界面上配一个保持寄存器地址 100、缩放 1.0 的点位，读回来应当正好是 100。

典型用法（在 Ubuntu 虚拟机里，先起一对虚拟串口，再跑模拟器）：
    socat -d -d PTY,raw,echo=0,link=/tmp/ttyS0 PTY,raw,echo=0,link=/tmp/ttyS1 &
    python3 modbus_rtu_sim.py /tmp/ttyS1 9600 8 N 1
    # 然后在软件里新增一台设备：协议选 "Modbus RTU"，
    # 地址填 /tmp/ttyS0，波特率/校验/停止位与上面一致。

依赖：sudo apt install python3-serial
      （Ubuntu 24.04 起 pip 受 PEP 668 保护，直接 `pip install pyserial` 会报
       externally-managed-environment，用 apt 装省事；一定要用 pip 就加
       --break-system-packages。）
"""

import argparse
import sys

try:
    import serial
except ImportError:
    sys.exit("缺少依赖 pyserial：请先 `sudo apt install python3-serial`"
             "（不要用 pip，Ubuntu 24.04 起会被 PEP 668 拦下）")

FC_READ_COILS = 0x01
FC_READ_HOLDING = 0x03
FC_READ_INPUT = 0x04


def crc16(data: bytes) -> int:
    """Modbus CRC16（多项式 0x8005，初值 0xFFFF，无反射）。返回小端两字节已就绪的整数。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 0x0001) else (crc >> 1)
    return crc


def build_frame(slave: int, pdu: bytes) -> bytes:
    frame = bytes([slave]) + pdu
    crc = crc16(frame)
    return frame + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def parse_request(frame: bytes):
    """拆出 [从站, 功能码, 起始地址, 数量]，帧不完整/CRC 错返回 None。"""
    if len(frame) < 4:
        return None
    crc_recv = (frame[-1] << 8) | frame[-2]
    if crc16(frame[:-2]) != crc_recv:
        return None
    slave = frame[0]
    fn = frame[1]
    start = (frame[2] << 8) | frame[3]
    count = (frame[4] << 8) | frame[5]
    return slave, fn, start, count


def make_response(slave: int, fn: int, start: int, count: int) -> bytes:
    """线圈返回按位打包；寄存器返回每个值 = 起始地址 + 偏移（便于人工核对）。"""
    if fn == FC_READ_COILS:
        byte_count = (count + 7) // 8
        bits = bytearray(byte_count)
        # 让偶数地址的线圈为 1，方便观察
        for i in range(count):
            if (start + i) % 2 == 0:
                bits[i // 8] |= 1 << (i % 8)
        return build_frame(slave, bytes([fn, byte_count]) + bytes(bits))
    if fn in (FC_READ_HOLDING, FC_READ_INPUT):
        out = bytearray([fn, count * 2])
        for i in range(count):
            value = (start + i) & 0xFFFF
            out += bytes([(value >> 8) & 0xFF, value & 0xFF])
        return build_frame(slave, bytes(out))
    # 不支持的功能码：返回异常响应（最高位 + 0x01 非法功能）
    return build_frame(slave, bytes([fn | 0x80, 0x01]))


def main() -> int:
    parser = argparse.ArgumentParser(description="Modbus RTU 从站模拟器（开发验证用）")
    parser.add_argument("port", help="串口设备名，例如 /tmp/ttyS1 或 COM3")
    parser.add_argument("baud", type=int, default=9600, nargs="?", help="波特率（默认 9600）")
    parser.add_argument("databits", type=int, default=8, nargs="?", help="数据位 5-8（默认 8）")
    parser.add_argument("parity", choices=["N", "O", "E"], default="N", nargs="?",
                        help="校验位 N/O/E（默认 N）")
    parser.add_argument("stopbits", type=int, default=1, nargs="?", help="停止位 1 或 2（默认 1）")
    args = parser.parse_args()

    parity_map = {"N": serial.PARITY_NONE, "O": serial.PARITY_ODD, "E": serial.PARITY_EVEN}
    stop_map = {1: serial.STOPBITS_ONE, 2: serial.STOPBITS_TWO}

    try:
        ser = serial.Serial(
            port=args.port,
            baudrate=args.baud,
            bytesize=args.databits,
            parity=parity_map[args.parity],
            stopbits=stop_map.get(args.stopbits, serial.STOPBITS_ONE),
            timeout=1.0,
        )
    except Exception as exc:  # noqa: BLE001 - 终端工具，直接打印退出
        print(f"打开串口失败 {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Modbus RTU 从站模拟器已启动：{args.port} {args.baud}/{args.databits}/"
          f"{args.parity}/{args.stopbits}（Ctrl+C 退出）")

    buffer = bytearray()
    try:
        while True:
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buffer += chunk
            # RTU 帧以静默间隔分隔；这里简单按"收到完整请求长度"尝试解析。
            # 读请求固定 8 字节（从站1+功能1+起始2+数量2+CRC2）。
            while len(buffer) >= 8:
                frame = bytes(buffer[:8])
                parsed = parse_request(frame)
                if parsed is None:
                    # CRC 或长度不对：丢掉一字节，避免错位死循环
                    del buffer[0]
                    continue
                slave, fn, start, count = parsed
                buffer = buffer[8:]
                if fn in (FC_READ_COILS, FC_READ_HOLDING, FC_READ_INPUT):
                    resp = make_response(slave, fn, start, count)
                    ser.write(resp)
                    print(f"  <- 读请求 FC={fn:#04x} 起始={start} 数量={count}  回应 {len(resp)} 字节")
                else:
                    print(f"  <- 不支持的功能码 FC={fn:#04x}（仅实现读）", file=sys.stderr)
    except KeyboardInterrupt:
        print("\n已退出")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
