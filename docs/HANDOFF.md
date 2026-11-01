# HANDOFF — PHY110（OPPO Find X7 Ultra / 6.1.141）越狱移植

写给下一个 agent。详细内容不重复，直接读文末引用的 artifacts。

## 一句话状态

CVE-2026-98260 已移植到 **PHY110 / ColorOS 16.1 / `6.1.141-android14-11-o-gb6893081c037`**
并在真机跑通：SELinux Permissive → 内核 `uid=0` + 全 CapEff → `ksud late-load --kmi android14-6.1`
→ KernelSU LKM Live → `su` 可用（`u:r:ksu:s0`）。
**当前正在排查**：root + KernelSU 状态下，切 USB「传输文件（MTP）」有概率内核 panic 重启。

## 仓库 / artifacts（不要重复其中已有的内容）

- 代码仓库：`X7U-jailbreak/`，已推送 **https://github.com/InsulateJustf/PHY110-JailBreak**（`main`）。
  提交历史以 `git log --oneline` 为准（移植、构建脚本、APK、prebuilt/kernel 入库、env 契约修复等）。
- `README.md`：状态表、构建方式、实测完整流程、ColorOS 安装绕法、已知问题。
- `docs/PORT-NOTES.md`：本机全部测量常量、6.1 与 6.18/6.12 的差异、踩坑记录。
- `kernel/README.md`：`vmlinux.elf` / `.nm` / `.btf` 的来源与交叉校验（`__ksymtab`、符号大小、slabinfo）。
- `prebuilt/README.md`：`t98260`、`rootdclient`、KernelSU v3.3.1 资产的 SHA-256 与许可。
- 未入库但已归档：`X7U-jailbreak/work/`（OTA 8.4 GB、payload/boot.img、`vmlinux.elf`、JDK/SDK、
  ksud/管理器 APK、探针源码与日志）。**gitignore 状态，不要提交。**

## 本机环境

- 设备：USB 直连的 PHY110（serial 见 `adb devices`，本文不记录）。
- NDK：`/opt/homebrew/share/android-ndk`（`aarch64-linux-android31-clang`）。
- 项目内工具链：`work/tools/jdk21`、`work/tools/android-sdk`；`local.properties` 指向后者。
- exploit 构建：
  ```bash
  cd exploit/rust-t98260
  ANDROID_NDK_HOME=/opt/homebrew/share/android-ndk \
  CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android31-clang \
  PYB110_TARGET=phy110-6.1 cargo build --release --target aarch64-linux-android
  ```
- App 构建：`bash tools/bootstrap-android.sh`（免 sudo；产物在 `app/app/build/outputs/apk/debug/`）。

## 设备端现状（接手时先核对）

- `/data/local/tmp/`：`t98260-p`（手工 plant）、`rootdclient`、`ksud`、
  `android14-6.1_kernelsu.ko`、`plant.log`、`kmsg.log`、`events.log`、`logcat*.log`。
- App 自己起的副本：`/data/local/tmp/phy110-<id>-<epoch>/t98260`（它会自行 `late-load`）。
- 已安装：`me.weishu.kernelsu`（管理器）、`com.phy110.onekey`（一键前端）。
- 一键前端仍缺前置：**Shizuku 服务要在跑**（首屏 `Stellar/Shizuku 未运行`）。

## 必须知道的操作事实（血泪）

1. **ColorOS 拒绝流式安装**：`adb install` / `pm install` → `Failure [-99]`；
   必须 `adb install --no-streaming -r <apk>`。
2. **`late-load` 成功后 SELinux 可能回到 Enforcing**（KernelSU 恢复），此时 rootd socket 对
   shell 不可达 → 用 `su -c`。但当前实测 `getenforce` 也可能是 `Permissive`（App 流程在
   late-load 后又跑过 selinux 阶段），不要假设，现查。
3. **不要杀 plant / rootd**：假 cred 释放与未回收僵尸都会 panic。驻留进程必须保活。
4. **LKM 不跨重启**：每次开机都要 `plant --auto`（~10–15 s）→ `ksud late-load --kmi android14-6.1`。
5. **App 会自己跑完整流程**（`t98260 __plant --auto` 然后 late-load），会污染手工 A/B；
   做对照实验前用 `pgrep -f t98260` 确认没有 App 起的实例。
6. 目标机 BL 锁定、无 fastboot、无 9008 免授权、已两次 ARB 熔断：**绝不刷机，只改内存**。
   用户已备份数据并接受售后风险。

## 未完成：MTP 切换概率重启（本次会话的最后任务）

已有证据：
- 干净启动（无 root、无 LKM）切 MTP：**稳定**。
- root + KernelSU 状态下切 MTP：**概率重启**。
- panic 为内核数据异常，栈尾：
  `do_mem_abort → do_bad_area → __do_kernel_fault → die → panic`（OPPO `[minidump]` 落盘）。
- 上一次 minidump 的故障头（`Unable to handle …` / `pc :` / `esr=`）被环形缓冲截断，暂无法指认。
- 已做的缓解：把所有驻留 pid（plant、rootd 及其子进程）的 `oom_score_adj` 设为 `-1000`，
  用于验证“MTP 传输造成内存压力 → LMK 杀驻留进程 → panic”这一假设。**等待用户复测结果**。

下一步（按顺序）：
1. 用户复测 MTP 2–3 次：
   - **稳定** → 确认是 LMK 杀驻留进程；把 `oom_score_adj=-1000` 固化进 plant 启动/App 流程并提交。
   - **仍崩** → 进入 2。
2. 取最新崩溃日志：`su -c` 读取 `/data/persist_log/DCS/de/minidump/SYSTEM_LAST_KMSG*`
   （gzip），grep `[kernelsu]`：
   - 出现 `[kernelsu]` 帧 → LKM 在厂商 6.1.141 上的兼容问题；评估换管理器的 KMI 构建、
     或限制 hook 面。
   - 无该帧、仍是 `do_mem_abort` → 驻留物延迟访问；改收尾策略：late-load 成功后让 plant
     **安全退役**（当前只会 park）。
3. 复现手段：**物理插拔 + 在选择“传输文件”**；软件 `svc usb setFunctions mtp,adb` 不触发。
4. 内核日志来源：OPPO minidump 是唯一可靠的（pstore 未注册；`su -c` 下后台
   `cat /dev/kmsg` 会被回收，脚本 `tools/capture-kmsg.sh` 只能短时生效）。

## 建议的第一个命令序列

```bash
adb devices
adb shell 'getenforce; grep -i kernelsu /proc/modules; su -c "id -u"'
adb shell "su -c 'ls -t /data/persist_log/DCS/de/minidump | head -3'"
cd X7U-jailbreak && git log --oneline -8 && sed -n 1,80p docs/PORT-NOTES.md
```

## Suggested skills

下一个 agent 应当调用 Skill 工具，至少指定：

1. **`diagnosing-bugs`** — 继续定位“MTP 切换概率重启”：建立症状→复现→假设→证据闭环，
   最小化二分（干净 boot / 只 root / root+KSU），并强制用真实日志而不是猜测。
2. **`codebase-design`** — 若决定重构“plant 安全退役 / 驻留进程保活 / LKM 收尾”这部分，
   把 exploit 的生命周期与收尾做成清晰接口（当前是隐式 park + 全局状态）。
3. **`tdd`** — 若要给“命中判定、收尾路径、retry 策略”加宿主侧可重复测试；当前只有真机验证，
   任何回归都要重启设备才能发现。

其余 skill（pr、research、grilling、domain-modeling、scaffold-exercises、migrate-to-shoehorn、
setup-pre-commit、wizard、prototype、writing-for-agents、git-guardrails）与本任务无关。
