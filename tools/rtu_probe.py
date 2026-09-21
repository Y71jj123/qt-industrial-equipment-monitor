#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Modbus RTU 链路探针（开发验证用，不是生产代码）。

作用：绕过 Qt 程序，直接用 pyserial 往串口发一帧标准 Modbus RTU 读请求，看有没有应答。
这样能把"读不到数"一刀切成两半：

  * 探针收到合法应答 → socat + 模拟器这条链路是好的，问题在 Qt 程序侧；
  * 探针也超时        → 问题在 socat / 模拟器侧，Qt 程序是无辜的。

默认参数与 README 的联调步骤对齐（/tmp/ttyS0、从站 1、保持寄存器 100）。

用法：
    python3 rtu_probe.py [串口] [从站] [起始地址] [数量]
    python3 rtu_probe.py                       # /tmp/ttyS0 1 100 1
返回码：0 收到合法应答；2 无应答；3 CRC 不符；4 打不开串口。
"""

import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少依赖 pyserial：sudo apt install python3-serial")


def crc16(data: bytes) -> int:
    """Modbus CRC16（多项式 0xA001 反射形式，初值 0xFFFF）。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def build_request(slave: int, start: int, count: int) -> bytes:
    """组一帧 FC03（读保持寄存器）请求：从站 + 功能码 + 起始 + 数量 + CRC(小端)。"""
    body = bytes([slave, 0x03,
                  (start >> 8) & 0xFF, start & 0xFF,
                  (count >> 8) & 0xFF, count & 0xFF])
    crc = crc16(body)
    return body + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ttyS0"
    slave = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    start = int(sys.argv[3]) if len(sys.argv) > 3 else 100
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 1

    request = build_request(slave, start, count)
    print(f"探针目标：{port}（从站 {slave}）  请求：读保持寄存器 起始={start} 数量={count}")
    print(f"发送：{request.hex(' ')}")

    try:
        ser = serial.Serial(port, baudrate=9600, bytesize=8, parity="N",
                            stopbits=1, timeout=0.3)
    except Exception as exc:  # noqa: BLE001 - 终端工具，直接打印
        print(f"打开串口失败：{exc}", file=sys.stderr)
        print("→ 这个设备名根本不存在或者打不开，先确认 socat 还在跑。")
        return 4

    data = b""
    try:
        ser.reset_input_buffer()
        ser.write(request)
        ser.flush()

        expected = 5 + 2 * count
        deadline = time.time() + 2.0
        while time.time() < deadline and len(data) < expected:
            chunk = ser.read(max(1, ser.in_waiting))
            if chunk:
                data += chunk
    finally:
        ser.close()

    if not data:
        print("没有任何应答（超时 2 秒）。")
        print("→ socat / 模拟器这条链路没通，Qt 程序是无辜的。逐项自查：")
        print("   1) socat 窗口还活着吗？（关掉链路就断）")
        print("   2) 模拟器窗口还在吗？有没有报 `打开串口失败`、或者已经退回命令行？")
        print("      （它必须挂在 /tmp/ttyS1 上，且要在 socat 启动之后再起）")
        print("   3) ls -l /tmp/ttyS0 /tmp/ttyS1 —— 两个软链是否指向真实存在的 /dev/pts/N")
        print("      （指向的文件带删不掉的红字 = 悬空软链，说明 socat 重启过，模拟器挂在旧口上）")
        return 2

    print(f"收到：{data.hex(' ')}")
    recv_crc = (data[-1] << 8) | data[-2]
    if crc16(data[:-2]) != recv_crc:
        print("CRC 校验不符 —— 报文被改写过，多半是 socat 少了 `raw,echo=0`。")
        return 3
    if len(data) >= 3 and (data[1] & 0x80):
        print(f"从站返回异常码 {data[2]}（功能码 {data[1]:#04x}）。")
        return 0

    regs = []
    if len(data) >= 3:
        for i in range(data[2] // 2):
            off = 3 + 2 * i
            if off + 1 < len(data) - 2:
                regs.append((data[off] << 8) | data[off + 1])
    print(f"解析：{len(regs)} 个寄存器 = {regs}")
    print("链路 OK —— socat + 模拟器都正常，说明问题在 Qt 程序侧。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
