# X7U Root — OPPO Find X7 Ultra (PHY110) 一键越狱工程

把 [OnePlus16-Jailbreak](../OnePlus16-Jailbreak) 的 **CVE-2026-98260** 引擎移植到
**OPPO Find X7 Ultra（PHY110，ColorOS 16.1，内核 `6.1.141-android14-11-o-gb6893081c037`）**，
并在原 Android App 基础上改造成本机型的一键前端。

> 仅用于自有设备的系统研究。目标机 BL 锁定、无 fastboot、无 9008 免授权，
> 且已越过 ColorOS 16 的两次不可逆熔断 —— **不要刷任何固件**，一切改动只发生在内存里。

## 真机验证状态（2026-11-01）

| 阶段 | 结果 | 证据 |
|---|---|---|
| 内核门禁 | ✅ | `PROBE_KERNEL=6.1.141-android14-11-o-gb6893081c037` / `PROBE_OK` |
| KASLR slide | ✅ | 由 `do_timer_create+0xa8` 的 ftrace 记录反推 |
| order-1 页回收 | ✅ | `/proc/slabinfo` 同步 + 预建 socket，批量抓取 64/67 页 |
| rb_erase 写原语 | ✅ | `NODE_MARKER=1`（节点被擦除）、`SELF_WRITE=1`（零写落点正确） |
| SELinux Permissive | ✅ | `selinux_state - 7` 零写，`getenforce` → `Permissive` |
| 内核态 uid 0 | ✅ | `Uid: 0 0 0 0`、`CapEff: 000001ffffffffff`、`id -u` = 0 |
| 持久 root（KernelSU） | ⏳ | 缺 `ksud late-load --kmi android14-6.1`（KMI 已确认） |

当前 root 是**进程级、重启失效**的；rootd 在 `/data/local/tmp/phy110-root.sock`。

## 目录

```
X7U-jailbreak/
├── exploit/
│   ├── src/t98260_plant.c          # C 引擎（6.18 路径保留在 #ifndef 后，6.1 走 TARGET_PHY110_61）
│   ├── src/t98260_phy110_61.h      # 本机全部常量（符号、结构体偏移、映射、时序）
│   └── rust-t98260/                # Rust 单入口：ksud 发现、late-load、install
├── tools/                          # 真机探针与构建脚本
│   ├── env.sh                      # NDK / PATH / adb serial
│   ├── extract.sh                  # OTA -> boot.img -> vmlinux.elf
│   ├── btf_dump.py                 # .BTF 解析（结构体偏移）
│   ├── rootdclient.c               # 连 rootd 执行 root 命令
│   └── {leakprobe,spraytest,pfntest,maptest,reclaimprobe}.c
├── app/                            # Android 一键前端（Shizuku/Stellar 驱动）
└── docs/PORT-NOTES.md              # 移植过程与踩坑记录
```

## 构建 exploit

需要 NDK（r26+，`aarch64-linux-android31-clang`）与 Rust 的 `aarch64-linux-android` target：

```bash
export ANDROID_NDK_HOME=/opt/homebrew/share/android-ndk
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android31-clang
cd exploit/rust-t98260
PYB110_TARGET=phy110-6.1 cargo build --release --target aarch64-linux-android
cp target/aarch64-linux-android/release/t98260 ../../app/app/src/main/assets/phy110/t98260
```

`PYB110_TARGET=phy110-6.1` 给 C 引擎加 `-DTARGET_PHY110_61`；不设时编译的是原 6.18 目标。

## 手工运行（不装 App）

```bash
adb push exploit/.../t98260 /data/local/tmp/t98260-p && adb shell chmod 755 /data/local/tmp/t98260-p
adb shell /data/local/tmp/t98260-p __plant --probe          # 只读
adb shell /data/local/tmp/t98260-p __plant --slide-probe    # 只读
adb shell /data/local/tmp/t98260-p __plant --auto           # 全链路，成功后会 park
adb push tools/rootdclient.c 编译产物 /data/local/tmp/
/data/local/tmp/rootdclient -c "id; getenforce"
```

`--selinux-only` 停在 Selinux 阶段，`--tile-self` 做无害自写校验。

## 一键 App

`app/` 已从原工程改造：

- 包名 `com.phy110.onekey`，asset 目录 `phy110/`，运行目录 `/data/local/tmp/phy110-*`；
- 漏洞选项只剩 `CVE-2026-98260-6.1`（没有 6.12 分支）；
- 默认 KMI `android14-6.1`（`parse_kmi("6.1.141-android14-11-o-…")` 的结果）；
- 管理器下拉保留 KernelSU / KernelSU Next / FinaSU / BakaSU / SukiSU Ultra；
- ksud 不随包内置，运行时从已安装管理器 APK 的 `lib/arm64` 原地执行（与原工程一致）。

构建前提（本机当前缺）：**JDK 21**、**Android SDK platform 37 + build-tools**、NDK。
Gradle wrapper 会自行下载 Gradle。示例：

```bash
brew install --cask temurin@21
sdkmanager "platforms;android-37" "build-tools;37.0.0"
cd app && ./gradlew :app:assembleDebug
```

## 已知问题

- **ColorOS 拒绝 `adb install` 流式安装**：实测 `Failure [-99]`。改用
  `adb push x.apk /data/local/tmp/ && adb shell pm install -r /data/local/tmp/x.apk`，
  或在设置里允许“USB 安装/未知来源”后再装。
- 第一次运行需要干净开机；失败路径会留下一个**不回收**的僵尸进程（这是防止
  `rb_erase` 打到别人页上的保护），下次运行前建议重启。
- 任何一次 reclaim 未命中都**不会** panic（会重试），但若命中检测误判仍可能崩；
  目标机无 fastboot / 9008，panic 只能等自动重启。

## 许可

本工程自研代码沿用原仓库 MIT；第三方（KernelSU GPL-2.0、SUSFS GPL-2.0 等）见原
`OnePlus16-Jailbreak/NOTICE`。
