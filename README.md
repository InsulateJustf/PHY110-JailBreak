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
| KernelSU LKM | ✅ | `ksud late-load --kmi android14-6.1` → `kernelsu 225280 0 - Live` |
| 持久 root（su） | ✅ | `su -c "id"` → `uid=0 context=u:r:ksu:s0`，SELinux 恢复 Enforcing |

重启后 LKM 不保留：每次开机由 App（或手工 `--auto`）重新跑一次漏洞再 `late-load`，
与上游 jailbreak 模式一致。

## 目录

```
X7U-jailbreak/
├── prebuilt/                       # 最小二进制集合（t98260 / rootdclient / KernelSU v3.3.1）
│   └── kernelsu/                   # ksud + android14-6.1_kernelsu.ko + 管理器 APK（GPL-2.0，原样）
├── kernel/                         # 目标内核镜像与符号表（vmlinux.elf / .nm / .btf）
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

构建前提：**JDK 21**、**Android SDK platform 37 + build-tools**。仓库自带免 sudo 的引导脚本，
会在 `work/tools/` 下装项目内 JDK/SDK 并直接出包（依赖已核对可解析：AGP 9.4.1、
Miuix 0.9.4、Compose BOM 2026.09.00、Stellar API 1.0.3、Gradle 9.7.1、platform 37）：

```bash
bash tools/bootstrap-android.sh     # -> app/app/build/outputs/apk/debug/
```

想要 release 包再补 signing：复制 `app/keystore.properties.example` 并跑
`./gradlew :app:assembleRelease`。

## 产物来源与未入库的大文件

`prebuilt/` 与 `kernel/` 的每个文件都带 SHA-256 与其来源（见各自 README）。未入库的是
体积超过 GitHub 单文件 100 MB 限制的中间产物，全部可由 `tools/extract.sh` 再生：

| 文件 | 大小 | 再生 |
|---|---|---|
| OTA 全量包 `PHY110_16.0.10.500.zip` | 8.4 GB | OPPO `update/v5` 现场换 token |
| `payload.bin` | 8.4 GB | 从 OTA zip 解出 |
| `boot.img` / `vendor_boot.img` | 各 192 MB | `payload_dumper --partitions boot,vendor_boot` |
| JDK / Android SDK / uv 工具链 | ~1.3 GB | `bash tools/bootstrap-android.sh` |

这些文件**不删**：它们已归档在本仓库工作区的 `work/`（`work/ota`、`work/out`、`work/bin`、
`work/kernelsu`、`work/src`、`work/tools`），只是被 `.gitignore` 排除在 git 之外。
`work/` 里同时保留了原工程的 `NOTES-old-repo.md` 与实机截图。

## 实测完整流程（2026-11-01）

```bash
# 0) 干净开机，安装管理器（流式安装会被 ColorOS 拒绝，必须 push 安装）
adb install --no-streaming -r KernelSU_v3.3.1_32680-release.apk

# 1) 后台跑漏洞，拿到内核 root 与 rootd
adb shell 'setsid /data/local/tmp/t98260-p __plant --auto >/data/local/tmp/plant.log 2>&1 </dev/null &'
# 日志出现 PLANT_CRED_OK / ROOTD_LISTEN 即可

# 2) 通过 rootd 载入 LKM（KMI android14-6.1）
/data/local/tmp/rootdclient -c "/data/local/tmp/ksud late-load --kmi android14-6.1 \
  --allow-shell --package-name me.weishu.kernelsu"

# 3) 验证：KernelSU 已接管，SELinux 回到 Enforcing
su -c "id; getenforce"
#   uid=0(root) ... context=u:r:ksu:s0
#   Enforcing
grep kernelsu /proc/modules     # kernelsu 225280 0 - Live
```

注意：`late-load` 成功后 SELinux 恢复 Enforcing，rootd socket 对 shell 不再可达，
后续一律用 `su -c`。

## 已知问题

- **ColorOS 拒绝 `adb install` 流式安装**：实测 `Failure [-99]`（`pm install` 同样失败）。
  **`adb install --no-streaming -r` 可绕过**，已实测成功。
- 第一次运行需要干净开机；失败路径会留下一个**不回收**的僵尸进程（这是防止
  `rb_erase` 打到别人页上的保护），下次运行前建议重启。
- 任何一次 reclaim 未命中都**不会** panic（会重试），但若命中检测误判仍可能崩；
  目标机无 fastboot / 9008，panic 只能等自动重启。

## 许可

本工程自研代码沿用原仓库 MIT；第三方（KernelSU GPL-2.0、SUSFS GPL-2.0 等）见原
`OnePlus16-Jailbreak/NOTICE`。
