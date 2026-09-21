#!/usr/bin/env bash
# =============================================================================
# build_on_ubuntu.sh —— 在 Ubuntu / Debian 上一条命令把项目跑起来
#
# 为什么要有这个脚本：本地开发在 Windows（MinGW），CI 在 GitHub 的 Ubuntu 上。
# 你想在自己的 Ubuntu 虚拟机里开发 / 演示，如果只是照着 README 手敲一堆 apt、
# cmake 参数，很容易漏一个依赖就卡半天。这个脚本把"装依赖 → 配置 → 构建 →
# 跑测试 → offscreen 冒烟"串成一行，且两种 Qt 来源都支持：
#
#   A. 发行版仓库的 Qt（最简，Ubuntu 24.04 自带 Qt 6.4.2，apt 一行装好）
#   B. Qt 官方在线安装器装到自定义目录（用 QT6_PREFIX 指定）
#
# 用法：
#   ./build_on_ubuntu.sh             # 默认 Debug + 跑测试 + offscreen 冒烟
#   ./build_on_ubuntu.sh --release   # Release 构建（出包 / 跑性能基线用）
#   ./build_on_ubuntu.sh --no-deps   # 跳过 apt（Qt 已就绪时省时间）
#   QT6_PREFIX=/opt/Qt/6.11.2/gcc_64 ./build_on_ubuntu.sh   # 用安装器装的 Qt
#
# 退出码约定（和 CI 一致）：
#   - 构建 / 测试失败 → 非零退出
#   - offscreen 冒烟里进程"活满 8 秒" → 被 timeout 杀掉，退出码 124 = 正常（没启动即崩）
# =============================================================================
set -euo pipefail

BUILD_TYPE=Debug
INSTALL_DEPS=1
for a in "$@"; do
  case "$a" in
    --release) BUILD_TYPE=Release ;;
    --no-deps) INSTALL_DEPS=0 ;;
    *) echo "::error:: 未知参数: $a" >&2; exit 1 ;;
  esac
done

# ---------------------------------------------------------------------------
# 1) 系统依赖 + Qt6
# ---------------------------------------------------------------------------
if [ "$INSTALL_DEPS" -eq 1 ]; then
  echo "==> 安装系统依赖与 Qt6（需要 sudo 密码）"
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    qt6-base-dev \
    qt6-base-dev-tools \
    qt6-tools-dev \
    qt6-tools-dev-tools \
    qt6-charts-dev \
    qt6-serialport-dev \
    libqt6sql6-sqlite \
    libgl1-mesa-dev \
    libopengl0 \
    libxkbcommon-x11-0 \
    libxcb-cursor0 \
    fonts-dejavu-core \
    fonts-wqy-zenhei \
    socat \
    python3-serial
  # 上面这些包分三类，缺一个就卡在不同地方，所以一次装齐：
  #
  #  ① 构建 / Qt：qt6-base-dev、qt6-charts-dev、qt6-serialport-dev
  #     qt6-serialport-dev 用来编 Modbus RTU（串口）；没它时 RTU 连接类因 HAVE_QT_SERIALPORT
  #     守卫自动不编译，主程序照常构建，只是设备协议下拉里没有 "Modbus RTU" 这一项。
  #  ② 运行时（被 --no-install-recommends 挡在门外的几个，最容易漏）：
  #     - libqt6sql6-sqlite   SQLite 驱动。缺它程序能启动，但一开数据库就报 "Driver not loaded"。
  #     - libxkbcommon-x11-0 / libxcb-cursor0
  #                            Qt 的 xcb 平台插件依赖。缺了 GUI 直接起不来，报
  #                            "could not load the Qt platform plugin xcb"。
  #                           （libxcb-cursor0 是 Qt ≥ 6.5 才强制要求的，发行版 6.4 还用不到，
  #                             但你要换安装器装的 Qt 就会撞上，先装上）
  #     - fonts-wqy-zenhei   中文字体。界面文案全是中文，没它满屏方块。
  #                           用文泉驿正黑而不是 fonts-noto-cjk：后者 100MB+，前者只要十几 MB。
  #  ③ RTU 联调工具：socat 造虚拟串口对；python3-serial 给模拟器用。
fi

# ---------------------------------------------------------------------------
# 2) 决定 Qt 路径（CMAKE_PREFIX_PATH）
# ---------------------------------------------------------------------------
if [ -n "${QT6_PREFIX:-}" ]; then
  PREFIX="$QT6_PREFIX"
  echo "==> 使用指定的 Qt: $PREFIX"
elif command -v qmake6 >/dev/null 2>&1; then
  PREFIX="$(dirname "$(dirname "$(readlink -f "$(command -v qmake6)")")")"
  echo "==> 使用系统 qmake6 找到的 Qt: $PREFIX"
else
  echo "::error:: 找不到 Qt6。请先 'sudo apt install qt6-base-dev'，或用 QT6_PREFIX=... 指定。" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 3) 配置 + 构建
# ---------------------------------------------------------------------------
BUILD_DIR="build-$(echo "$BUILD_TYPE" | tr 'A-Z' 'a-z')"
cmake -S . -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$PREFIX"

cmake --build "$BUILD_DIR" --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# 4) 单元测试
# ---------------------------------------------------------------------------
echo "==> 单元测试"
ctest --test-dir "$BUILD_DIR" --output-on-failure

# ---------------------------------------------------------------------------
# 5) offscreen 冒烟：进程活满 8 秒 = 没启动即崩
#    （GUI 程序在 CI / 无显示器的 VM 里会停在登录框，timeout 杀掉 = 活着）
# ---------------------------------------------------------------------------
BIN="$BUILD_DIR/src/qt-industrial-equipment-monitor"
if [ -x "$BIN" ]; then
  echo "==> offscreen 冒烟（进程应活满 8 秒）"
  set +e
  QT_QPA_PLATFORM=offscreen timeout 8 "$BIN"
  code=$?
  set -e
  if [ "$code" -ne 124 ]; then
    echo "::error:: 进程提前退出 (exit=$code) —— 启动阶段就崩了" >&2
    exit 1
  fi
  echo "==> 冒烟通过：进程存活满 8 秒 ✅"
fi

echo ""
echo "构建完成。产物在 $BUILD_DIR/"
echo "  - 可执行文件: $BIN"
echo "  - 协议插件:   $BUILD_DIR/src/plugins/protocols/*.so"
echo ""
echo "有显示器时直接运行："
echo "  $BIN"
echo "无显示器（服务器 / 容器 / CI）："
echo "  QT_QPA_PLATFORM=offscreen $BIN"
