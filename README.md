# ozj-appdata

Android 应用数据 **打包 / 恢复** 命令行工具（C++17，需 root）。

参照 [Android-DataBackup-custom](https://github.com/boluo4169-commits/Android-DataBackup-custom)（v3.15.2）的
备份/恢复流程实现，但去掉 Kotlin/Gradle 依赖，做成一个静态小体积可执行文件：
`tar` 的读写由本工具自己实现（ustar + pax 扩展头），不依赖系统 `tar`；
压缩支持 **zstd** / gzip / 不压缩，其中 zstd 与 DataBackup 的 `.tar.zst` 完全互通。

**与 DataBackup 双向兼容**：`--format databackup` 直接生成它认识的目录结构（含 `package_restore_config.json`
与 `.md5`），它能识别并恢复；反过来，本工具也能直接恢复它生成的备份目录。

---

## 一、上游是怎么打包应用数据的

DataBackup 走的是 **root 直接读写 `/data`** 的路线，不是 `adb backup` 也不是 `pm` 的备份通道。

### 1. 五类数据目录

| 类型 | 目录 |
| --- | --- |
| `user` | `/data/user/<N>/<pkg>` |
| `user_de` | `/data/user_de/<N>/<pkg>` |
| `data` | `/data/media/<N>/Android/data/<pkg>` |
| `obb` | `/data/media/<N>/Android/obb/<pkg>` |
| `media` | `/data/media/<N>/Android/media/<pkg>` |

`<N>` 是 Android 多用户 id（主用户 0）。APK 单独一类，取 `pm path <pkg>` 返回路径的**父目录**，
再把该目录顶层的 `*.apk` 全部收进来（base.apk + split apk）。

### 2. 打包命令

`TarUtil.compress` 实际执行的是：

```
cd <源目录> && tar --totals <排除参数> -cpf - ./<包名> | zstd -r -q > <目标>.tar.zst
```

APK 那一路是 `tar -C <apk 父目录> -cpf - ./*.apk`，所以 **apk.tar 的顶层直接就是 `./base.apk`**，
没有包名目录这一层。

要点：
- 归档内顶层就是**包名**，恢复时解到对应目录即可，不需要额外的路径映射表。
- `-c` 创建、`-p` 保留权限位、`-f -` 走管道交给 zstd。
- 压缩器可切换（TAR / ZSTD / LZ4），归档后缀随之为 `.tar` / `.tar.zst` / `.tar.lz4`。

### 3. 备份目录结构

```
<saveDir>/apps/<label>_<pkg>/user_<uid>@<preserveId>/
├── package_restore_config.json     # Gson 序列化的 PackageEntity（包名、版本、勾选状态、体积…）
├── apk.tar.zst                     # 顶层 ./base.apk、./split_config.arm64_v8a.apk
├── user.tar.zst
├── user_de.tar.zst
├── data.tar.zst
├── obb.tar.zst
├── media.tar.zst
└── <每个归档>.md5                   # 恢复前校验完整性
```

`preserveId` 是保留版本的毫秒时间戳；主备份（最新一份）用 `user_<uid>` 不带 `@`。
`<label>_` 前缀可以省略，扫描时对纯包名目录（legacy 格式）同样识别。

### 4. 排除项（`PackagesBackupUtil.buildExclusionAndPrune`）

```
user / user_de :  <pkg>/.ota  <pkg>/cache  <pkg>/lib  <pkg>/code_cache  <pkg>/no_backup
data/obb/media :  <pkg>/cache  + 通配 Backup_*  .trashed-*  .pending-*  .Trashed-*  .Pending-*
```

排除 `cache` / `code_cache` 是因为这些是**新机上必然会重新生成**的东西，带过去只会带来
dex/oat 与 ABI 不匹配；`lib` 同理（原生库由 APK 安装时按目标 ABI 释放）；
`Backup_*`、`.trashed-*`、`.pending-*` 是微信/QQ 之类的临时目录，体积大且无意义
（`trashed`/`pending` 是 AOSP MediaProvider 的回收站与写入中前缀，大小写都匹配）。

### 5. 打包前可选杀进程

`KillAppOption`：`DISABLED` / `kill -9 <pid>` / AMS `forceStopPackageAsUser`。
不杀进程直接拷会拿到写到一半的 db（SQLite WAL 与主库不一致），恢复后大概率闪退或丢数据。

---

## 二、上游是怎么恢复的（这段决定了"能不能不闪退"）

`PackagesRestoreUtil.restoreData` 的顺序：

1. 先 `force-stop` 掉目标应用（避免旧进程持有 db / dex / so）。
2. `pm install` 装 APK：**只有 1 个 apk 时才直接装**；base + split 多个时走
   `pm install-create` → 逐个 `pm install-write` → `pm install-commit` 会话安装。
   参数按 SDK 分支：34+ 加 `--bypass-low-target-sdk-block`，30+ 加 `-i com.android.vending`，
   统一 `--user N -r -t -d`。装完轮询确认包真的落库。
3. **查新机上的 uid**：轮询 `pm list packages -U --user N`，最多 20 次 × 500ms。
   新机的 uid 和备份机的 uid 通常不一样（10123 → 10234 这种），不重映射必然失败。
4. **解包前先读目标目录当前的 SELinux context**（`ls -Zd <目录>`）。
   目录是 installd 按**本机 uid** 建的，其 MCS 分类（`:c512,c768` 这种）跟备份机可能不同，
   照备份机的 context 打标签会让 App 直接失去访问权。
5. 可选「干净恢复」：清空目标目录第一层，但保留 `lib` `cache` `code_cache` `no_backup` `.ota`。
   （不用 `--recursive-unlink`，因为它会把新机已经释放好的 lib 一起删掉。）
6. 解包：`tar --totals -xpf <归档> -C <目标父目录>`。
   **注意是父目录**：归档内顶层就是 `<pkg>/`，`-C` 指到 `/data/user/0` 才对；
   指到 `/data/user/0/<pkg>` 会解出 `<pkg>/<pkg>/...`，应用读不到数据。
   排除项（与备份侧不同，恢复侧更严格，五类都排除）：
   `<pkg>/.ota` `<pkg>/cache` `<pkg>/lib` `<pkg>/code_cache` `<pkg>/no_backup`，
   外部存储再加 `Backup_*` `.trashed-*` `.pending-*`（大小写两种写法）。
   `-p` 保留权限位；不加 `-m`，保留 mtime（很多 App 靠 mtime 判断缓存新鲜度，游戏反作弊也看）。
7. `chown -hR <新uid>:<gid> <目标目录>`。
   `-h` 不跟随符号链接（App 私有目录里符号链接很多，跟随会 chown 到目标机上别的文件）。
   **gid 分两种**：`/data/user*` 用 uid 当 gid；`/data/media/Android/*` 是 FUSE/media_rw，
   gid 必须取**目标机父目录（即 data/obb/media 根）现有的 gid**，套用 uid 会导致 App 读写外部目录 EACCES。
8. **SELinux 重打标签**，两条分支：
   - `data/obb/media`（外部存储）：无条件 `restorecon -R -F <目录>`。
     这两处目录常被 MediaProvider 提前建出来，label 可能是 `media_rw_data_file:s0`（没有 category），
     照着快照 chcon 会把整树打错；交给系统按 `file_contexts` 重打才稳。
   - `user/user_de`：用**第 4 步记下的目标目录 context** 做 `chcon -hR`（含正确的 MCS 分类）；
     context 为空时兜底取父目录 context，把 `system_data_file` 替换成 `app_data_file`。
9. **属主校验**：`find <目录> ! -user <uid> | wc -l`，非 0 就再 chown 一次（FUSE 下 chown 偶发失效）。
10. 恢复 appops / 运行时权限 / SSAID（可选）。

每个归档解包前还会校验 `<归档>.md5`（`ChecksumUtil.verify`）：缺失或算不出就放行，
不一致则中止并让用户决定是否强制恢复。

### 所以"换机恢复不闪退"的四个必要条件

| 条件 | 不做会怎样 |
| --- | --- |
| uid/gid 重映射 | App 打不开自己的私有目录 → `EACCES` → 启动即崩 |
| SELinux context（含 MCS 分类，且要用目标机的） | `avc: denied`，同样是权限问题，但报错更隐蔽 |
| 权限位（目录 771 / 文件 660） | SQLite 无法建 `-wal`/`-journal`，打开 db 失败 |
| 包已装好 + 不带旧 cache/code_cache/lib | dex/oat 与 ABI 不匹配，`UnsatisfiedLinkError` / dex 校验失败 |

本工具把这四件事全部照做，另外加了路径穿越防护、硬链接去重和归档 MD5 校验。

### v1.2.0 按上游逐条核对后修掉的偏差

对着 `Android-DataBackup-custom`（`main`，含 `PackagesRestoreUtil` / `PackagesBackupUtil` /
`SELinux` / `Pm` / `ChecksumUtil` / `PathUtil` / `DataTypeUtil`）逐条比过，以下是与上游不一致、
且**直接影响恢复后能否正常打开**的问题，已全部改齐：

| # | 偏差 | 后果 | 修正 |
| --- | --- | --- | --- |
| 1 | 解包到 `<pkg>` 目录本身，而归档内是 `<pkg>/...` | 解出 `<pkg>/<pkg>/...`，数据等于没恢复 | 解到**父目录**（`extract_root_for`） |
| 2 | 恢复侧只排除 `cache/code_cache/no_backup` | 归档里的旧 `lib/*.so` 覆盖新装 APK 的 native 库 → `UnsatisfiedLinkError` 闪退 | 五类统一排除 `.ota/cache/lib/code_cache/no_backup` |
| 3 | 恢复侧没排除 `Backup_*` / 回收站文件 | 应用自己的旧备份、已删照片被复活 | 外部存储补 `Backup_*` + 4 种 trashed/pending |
| 4 | 未校验 `.md5` | 坏归档静默恢复，必闪退 | 恢复前校验，可 `--no-checksum` / `--force-checksum` |
| 5 | 多 APK 逐个 `pm install` | split 安装必失败，包残缺 | `install-create/write/commit` 会话安装 |
| 6 | `pm install` 缺 `-t`、`-i`、SDK34+ 的 bypass | 低 targetSdk / 带 testOnly 的包装不上 | 按 SDK 分支补齐（与 `PmUtil` 一致） |
| 7 | 外部存储 gid 取目标目录自身 | 目录新建时 gid=0 → 应用写不进外部存储 | 取**父目录**（`data/obb/media` 根）的 gid |
| 8 | 安装/停进程顺序颠倒 | 旧进程可能持有 db / dex | 先 `force-stop` 再装 APK |
| 9 | `--in` 只认版本目录 | 指到备份根目录就报错 | 根目录 / 应用目录 / 版本目录都能自动下探 |

`tests/roundtrip.cpp` 里对 1、2、3、7 有可执行断言（构造带 `lib/.ota/cache/Backup_/trashed` 的归档，
按恢复规则解到父目录，逐条验证结果），不靠人工比对。

---

## 三、本工具

### 与上游的差异

| 项 | DataBackup | ozj-appdata |
| --- | --- | --- |
| 形态 | Android App（Kotlin + Compose） | 单个 C++ 可执行文件（约 1 MB，静态链 zstd） |
| tar | 调用系统 `tar` | 自己实现 ustar + pax 读写 |
| 压缩 | zstd / gzip / none | zstd / gzip / none，且**能读写它的 `.tar.zst`** |
| 入口 | GUI | 命令行 |
| 目录结构 | `apps/<label>_<pkg>/user_<uid>@<ts>/` | 默认自有结构，`--format databackup` 时完全对齐上游 |
| 额外 | — | 路径穿越防护、硬链接去重、MD5 校验、属主二次校验 |

自实现 tar 的好处：不依赖 toybox/tar 的版本差异（Android 上 `tar` 对 pax 长路径、
`--totals` 的支持各家 ROM 不一致），也不怕 SELinux 拦 `exec` 系统 tar。

### 构建

```bash
# 主机（会跑往返自测）
sudo apt install -y cmake g++ zlib1g-dev libzstd-dev zstd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ozj-appdata-test

# Android（NDK r27+，zstd 用源码静态编进来）
curl -sL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz | tar xz -C /tmp
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DOZJ_BUILD_TESTS=OFF -DOZJ_ZSTD_SRC=/tmp/zstd-1.5.6
cmake --build build-arm64 -j
```

`-DOZJ_WITH_ZSTD=OFF` 可关掉 zstd（体积更小，但读不了 `.tar.zst`）。
CI 会产出 `arm64-v8a` / `armeabi-v7a` / `x86_64` 三个 ABI 的 zip。

### 用法

```
ozj-appdata backup  --pkg <包名> [--user 0] [--types user,user_de,data,obb,media,apk]
                    [--out DIR] [--compress zstd|gzip|none] [--kill none|am|kill]
                    [--format ozj|databackup]
ozj-appdata restore --in <备份目录> [--pkg <包名>] [--user 0] [--types ...]
                    [--clean] [--install-apk] [--no-selinux] [--no-verify]
                    [--no-checksum] [--force-checksum]
ozj-appdata list    --in <备份目录> [--pkg <包名>]
ozj-appdata info    --pkg <包名> [--user 0]
```

#### 备份

```bash
# 自有格式（默认 zstd）
su -c './ozj-appdata backup --pkg com.tencent.mm --out /sdcard/ozj_backup --kill am'

# 直接写成 DataBackup 能识别的结构
su -c './ozj-appdata backup --pkg com.tencent.mm --out /sdcard/DataBackup --format databackup'
```

自有格式产出：

```
/sdcard/ozj_backup/com.tencent.mm_20260101_120000/
├── meta.json          包名 / user / 原 uid / 各类目录的 context 与 gid / 文件数 / 大小
├── apk.tar.zst
├── user.tar.zst
├── user_de.tar.zst
├── data.tar.zst
├── obb.tar.zst
└── media.tar.zst
```

`--format databackup` 产出 `apps/<pkg>/user_<uid>@<毫秒时间戳>/` + `package_restore_config.json` + 每个归档的 `.md5`，
APK 归档按上游习惯打成顶层 `./*.apk`。DataBackup 打开「恢复」列表（或从设置里重新扫描本地备份）就能看到这一份。

#### 恢复（换机）

```bash
# 恢复本工具的备份
su -c './ozj-appdata restore --in /sdcard/ozj_backup/com.tencent.mm_20260101_120000 --install-apk --clean'

# 直接恢复 DataBackup 的备份（版本目录 / 应用目录 / 备份根目录三种都行，自动下探到最新一份）
su -c './ozj-appdata restore --in /sdcard/DataBackup --pkg com.tencent.mm --install-apk --clean'
su -c './ozj-appdata restore --in /sdcard/DataBackup/apps/微信_com.tencent.mm --install-apk --clean'
```

流程：force-stop → 装 APK（split 走会话）→ 轮询新 uid → **先记目标目录 context 再决定是否 mkdir**
→ 清空目标（保留 lib/cache/code_cache/no_backup/.ota）→ 校验 md5 → 解包到**父目录**
（跳过 .ota/cache/lib/code_cache/no_backup，外部存储再加 Backup_*/trashed）→ `chown -hR 新uid:gid`
→ SELinux 重打标签 → 属主二次校验。全程打印每一步结果，任何一步失败都会明确告警而不是静默继续。

`--in` 指向备份根目录时若有多份应用备份，必须用 `--pkg` 指定，否则会直接报错列出来，不会猜。

#### 其它

```bash
su -c './ozj-appdata info --pkg com.tencent.mm'        # 看目录、uid、gid、context
su -c './ozj-appdata list --in /sdcard/ozj_backup/xxx' # 看备份内容（两种格式都支持）
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部成功 |
| 1 | 部分类型失败（其余已完成，日志里有明细） |
| 2 | 参数错误 |

### 注意

- 必须 root（要写 `/data/user*`、`chown`、`chcon`）。
- 备份目录别放在应用自己的私有目录里，否则恢复时会被 `--clean` 一起清掉。
- `--kill am` 需要 `am` 可用；`--kill kill` 走 `pidof`，更通用但可能漏掉子进程。
- 恢复外部存储（`data/obb/media`）依赖目标机已挂载 FUSE；`/data/media` 未挂载时会写到裸目录，
  这种情况下 gid 会退化成 uid，需要手动 `restorecon`。
- 与 DataBackup 互通的边界：本工具不写它的 Room 数据库，靠的是它「从本地重新扫描」时读
  `package_restore_config.json`。若目标机上的 DataBackup 版本改了 `PackageEntity` 字段名，
  扫描会失败并回退到按目录名建记录（此时包名、体积等显示信息可能不准，但归档本身仍可恢复）。
