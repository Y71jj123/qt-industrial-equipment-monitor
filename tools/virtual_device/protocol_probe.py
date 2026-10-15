#!/usr/bin/env python3
"""虚拟设备的协议探针 —— 用真实 Modbus TCP 报文验证从站实现。

为什么不用 wireshark / 手工点界面：本项目对"验证"的一贯要求是**可复现、可断言**。
这个脚本只用标准库，直接发原始报文字节并对响应做断言，任何人一条命令就能重跑。

用法：
    # 1) 先起虚拟设备（自监听）：
    #    ./build/bin/virtual_device --auto-start --port 1502
    # 2) 再跑本脚本：
    python tools/virtual_device/protocol_probe.py
    python tools/virtual_device/protocol_probe.py --host 127.0.0.1 --port 1502

覆盖点：
    0x03 读保持寄存器 / 0x04 读输入寄存器 / 0x01 读线圈
    0x06 写单寄存器 / 0x05 写单线圈（含"写入后保持"）
    异常响应（非法地址 0x02 / 非法功能码 0x01）
    MBAP 事务号与单元号回填
"""

import argparse
import socket
import struct
import sys


def transact(sock, tid, unit, pdu):
    """发一个请求，按 MBAP 长度字段收回完整响应，返回 (事务号, 单元号, PDU)。"""
    sock.sendall(struct.pack(">HHHB", tid, 0, 1 + len(pdu), unit) + pdu)

    header = b""
    while len(header) < 7:
        chunk = sock.recv(7 - len(header))
        if not chunk:
            raise RuntimeError("连接被对端关闭")
        header += chunk
    tx, _proto, length, unit_id = struct.unpack(">HHHB", header)

    body = b""
    while len(body) < length - 1:
        chunk = sock.recv(length - 1 - len(body))
        if not chunk:
            raise RuntimeError("连接被对端关闭")
        body += chunk
    return tx, unit_id, body


def main():
    parser = argparse.ArgumentParser(description="虚拟设备 Modbus TCP 协议探针")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1502)
    args = parser.parse_args()

    failures = []

    def check(name, condition, detail=""):
        print(("  PASS  " if condition else "  FAIL  ") + name + ("" if condition else "   " + detail))
        if not condition:
            failures.append(name)

    sock = socket.create_connection((args.host, args.port), timeout=4)
    print("== connected to %s:%d ==" % (args.host, args.port))

    # 1) 读保持寄存器 0..4（主程序默认点位表就是这 5 个地址）
    _tx, _u, resp = transact(sock, 1, 1, struct.pack(">BHH", 3, 0, 5))
    values = struct.unpack(">5H", resp[2:12]) if len(resp) >= 12 else None
    print("\n[1] read holding registers 0..4 -> fn=%s byteCount=%s values=%s"
          % (resp[0], resp[1], values))
    check("fn=0x03", resp[0] == 3, hex(resp[0]))
    check("byteCount=10", resp[1] == 10, str(resp[1]))
    check("5 个寄存器返回", len(resp) == 12, str(len(resp)))
    check("温度在 [25,68]", bool(values) and 25 <= values[0] <= 68, str(values))
    check("压力在 [30,85]", bool(values) and 30 <= values[1] <= 85, str(values))
    check("转速在 [30,88]", bool(values) and 30 <= values[2] <= 88, str(values))
    check("振动在 [10,60]", bool(values) and 10 <= values[4] <= 60, str(values))

    # 2) 读线圈 3（运行状态）
    _tx, _u, resp = transact(sock, 2, 1, struct.pack(">BHH", 1, 3, 1))
    print("\n[2] read coil 3 -> fn=%s byteCount=%s bit=%s" % (resp[0], resp[1], resp[2] & 1))
    check("fn=0x01", resp[0] == 1, hex(resp[0]))
    check("线圈位为 0/1", (resp[2] & 1) in (0, 1))

    # 3) 读输入寄存器
    _tx, _u, resp = transact(sock, 3, 1, struct.pack(">BHH", 4, 0, 2))
    print("\n[3] read input registers 0..1 -> fn=%s byteCount=%s" % (resp[0], resp[1]))
    check("fn=0x04", resp[0] == 4, hex(resp[0]))

    # 4) 写单寄存器 1 = 999，再回读（验证"写入后保持"）
    _tx, _u, resp = transact(sock, 4, 1, struct.pack(">BHH", 6, 1, 999))
    print("\n[4] write single register[1]=999 -> echo=%s" % resp.hex())
    check("fn=0x06 原样回显",
          resp[0] == 6 and struct.unpack(">H", resp[3:5])[0] == 999)
    _tx, _u, resp = transact(sock, 5, 1, struct.pack(">BHH", 3, 1, 1))
    written = struct.unpack(">H", resp[2:4])[0]
    print("    read back register[1] = %d" % written)
    check("写入值被保持（不会被自动波动覆盖）", written == 999, str(written))

    # 5) 写单线圈 3 = ON，再回读
    _tx, _u, resp = transact(sock, 6, 1, struct.pack(">BHH", 5, 3, 0xFF00))
    print("\n[5] write coil[3]=ON -> echo=%s" % resp.hex())
    check("fn=0x05 原样回显", resp[0] == 5)
    _tx, _u, resp = transact(sock, 7, 1, struct.pack(">BHH", 1, 3, 1))
    print("    read back coil[3] bit = %d" % (resp[2] & 1))
    check("线圈写入被保持", (resp[2] & 1) == 1)

    # 6) 越界地址 -> 异常码 0x02
    _tx, _u, resp = transact(sock, 8, 1, struct.pack(">BHH", 3, 200, 1))
    print("\n[6] read out-of-range address -> %s" % resp.hex())
    check("异常响应 83 02（非法数据地址）", resp == bytes([0x83, 0x02]), resp.hex())

    # 7) 未支持功能码 -> 异常码 0x01
    _tx, _u, resp = transact(sock, 9, 1, struct.pack(">BHH", 0x10, 0, 1))
    print("\n[7] unsupported function 0x10 -> %s" % resp.hex())
    check("异常响应 90 01（非法功能码）", resp == bytes([0x90, 0x01]), resp.hex())

    # 8) MBAP 事务号 / 单元号回填
    tx, unit_id, _resp = transact(sock, 0xBEEF, 7, struct.pack(">BHH", 3, 0, 1))
    check("事务号回填", tx == 0xBEEF, hex(tx))
    check("单元号回填", unit_id == 7, str(unit_id))

    sock.close()

    total = 19
    print("\n==== %d 项断言，失败 %d 项 ====" % (total, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
