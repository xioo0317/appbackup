# ozj-appdata

Android 应用数据 **打包 / 恢复** 命令行工具（C++17，需 root）。

参照 [Android-DataBackup-custom](https://github.com/boluo4169-commits/Android-DataBackup-custom)（v3.15.2）的
备份/恢复流程实现，但去掉 Kotlin/Gradle 依赖，做成一个静态小体积可执行文件：
`tar` 的读写由本工具自己实现（ustar + pax 扩展头），不再依赖系统 `tar` 命令；
压缩用 zlib gzip（NDK 自带），替代上游的 zstd。

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
cd <源目录的父目录> && tar --totals <排除参数> -cpf - <包名> | zstd > <目标>.tar.zst
```

要点：
- `cd` 到父目录，归档内顶层就是**包名**，恢复时解到对应目录即可，不需要额外的路径映射表。
- `-c` 创建、`-p` 保留权限位、`-f -` 走管道交给 zstd。
- 压缩器可替换（zstd / gzip / none），恢复时按扩展名识别。

### 3. 排除项（`PackagesBackupUtil.buildExclusionAndPrune`）

```
user / user_de :  <pkg>/.ota  <pkg>/cache  <pkg>/lib  <pkg>/code_cache  <pkg>/no_backup
data/obb/media :  <pkg>/cache  + 通配 Backup_*  .trashed-*  .pending-*
```

排除 `cache` / `code_cache` 是因为这些是**新机上必然会重新生成**的东西，带过去只会带来
dex/oat 与 ABI 不匹配；`lib` 同理（原生库由 APK 安装时按目标 ABI 释放）；
`Backup_*`、`.trashed-*`、`.pending-*` 是微信/QQ 之类的临时目录，体积大且无意义。

### 4. 打包前可选杀进程

`KillAppOption`：`DISABLED` / `kill -9 <pid>` / AMS `forceStopPackageAsUser`。
不杀进程直接拷会拿到写到一半的 db（SQLite WAL 与主库不一致），恢复后大概率闪退或丢数据。

---

## 二、上游是怎么恢复的（这段决定了"能不能不闪退"）

`PackagesRestoreUtil.restoreData` 的顺序：

1. `pm install` 装 APK（split 逐个装），等 PackageManager 真正落库。
2. **查新机上的 uid**：轮询 `pm list packages -U --user N`，最多 20 次 × 500ms。
   新机的 uid 和备份机的 uid 通常不一样（10123 → 10234 这种），不重映射必然失败。
3. **记下目标目录现有的 SELinux context**（`ls -Zd <目录>`）和现有 gid。
4. 可选「干净恢复」：清空目标目录第一层，但保留 `lib` `cache` `code_cache` `no_backup` `.ota`。
   （等价 `--recursive-unlink`，但不把新机已经释放好的 lib 删掉。）
5. `tar --totals -xmpf <归档> -C <目标目录>` 解包。
   `-p` 保留权限位；不加 `-m` 所以保留 mtime（很多 App 靠 mtime 判断缓存新鲜度）。
6. `chown -hR <新uid>:<gid> <目标目录>`。
   `-h` 不跟随符号链接（App 私有目录里符号链接很多，跟随会 chown 到目标机上别的文件）。
   **gid 分两种**：`/data/user*` 用 uid 当 gid；`/data/media/Android/*` 是 FUSE/media_rw，
   gid 必须沿用**目标目录现有的 gid**，套用 uid 会导致 App 读写外部目录 EACCES。
7. **SELinux 重打标签**，两条分支：
   - `data/obb/media`（外部存储）：无条件 `restorecon -R -F <目录>`，
     label 由系统 `file_contexts` 决定，跟着目标机走才对。
   - `user/user_de`：用备份时快照下来的 context 做 `chcon -hR`（保 MCS 分类，
     形如 `u:object_r:app_data_file:s0:c512,c768`，两个 App 的 category 不同）；
     快照缺失时兜底取父目录 context，把 `system_data_file` 替换成 `app_data_file`。
8. **属主校验**：`find <目录> ! -user <uid> | wc -l`，非 0 就再 chown 一次。
9. 恢复 appops / SSAID（可选）。

### 所以"换机恢复不闪退"的四个必要条件

| 条件 | 不做会怎样 |
| --- | --- |
| uid/gid 重映射 | App 打不开自己的私有目录 → `EACCES` → 启动即崩 |
| SELinux context（含 MCS category） | `avc: denied`，同样是权限问题，但报错更隐蔽 |
| 权限位（目录 771 / 文件 660） | SQLite 无法建 `-wal`/`-journal`，打开 db 失败 |
| 包已装好 + 不带旧 cache/code_cache/lib | dex/oat 与 ABI 不匹配，`UnsatisfiedLinkError` / dex 校验失败 |

本工具把这四件事全部照做，另外加了路径穿越防护和硬链接去重。

---

## 三、本工具

### 与上游的差异

| 项 | DataBackup | ozj-appdata |
| --- | --- | --- |
| 形态 | Android App（Kotlin + Compose） | 单个 C++ 可执行文件（约 100 KB） |
| tar | 调用系统 `tar` | 自己实现 ustar + pax 读写 |
| 压缩 | zstd（默认）/ gzip / none | gzip / none（NDK 无 zstd） |
| 入口 | GUI | 命令行 |
| 排除项 / uid 重映射 / SELinux 处理 | — | 与上游逐条对齐 |
| 额外 | — | 路径穿越防护、硬链接去重、`--clean` 保留名单、属主二次校验 |

自实现 tar 的好处：不依赖 toybox/tar 的版本差异（Android 上 `tar` 对 pax 长路径、
`--totals` 的支持各家 ROM 不一致），也不怕 SELinux 拦 `exec` 系统 tar。

### 构建

```bash
# 主机（会跑往返自测）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ozj-appdata-test

# Android（NDK r27+）
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DOZJ_BUILD_TESTS=OFF
cmake --build build-arm64 -j
```

CI 会产出 `arm64-v8a` / `armeabi-v7a` / `x86_64` 三个 ABI 的 zip。

### 用法

```
ozj-appdata backup  --pkg <包名> [--user 0] [--types user,user_de,data,obb,media,apk]
                    [--out DIR] [--compress gzip|none] [--kill none|am|kill]
ozj-appdata restore --in <备份目录> [--pkg <包名>] [--user 0] [--types ...]
                    [--clean] [--install-apk] [--no-selinux] [--no-verify]
ozj-appdata list    --in <备份目录>
ozj-appdata info    --pkg <包名> [--user 0]
```

#### 备份

```bash
su -c './ozj-appdata backup --pkg com.tencent.mm --out /sdcard/ozj_backup --kill am'
```

产出：

```
/sdcard/ozj_backup/com.tencent.mm_20260101_120000/
├── meta.json          包名 / user / 原 uid / 各类目录的 context 与 gid / 文件数 / 大小
├── user.tar.gz        /data/user/0/com.tencent.mm
├── user_de.tar.gz     /data/user_de/0/com.tencent.mm
├── data.tar.gz        /sdcard/Android/data/com.tencent.mm
├── obb.tar.gz
├── media.tar.gz
└── apk.tar.gz         base.apk + split apk
```

`meta.json` 里的 `context` 是恢复时 `chcon` 的依据，**别删**。

#### 恢复（换机）

```bash
su -c './ozj-appdata restore --in /sdcard/ozj_backup/com.tencent.mm_20260101_120000 --install-apk --clean'
```

流程：装 APK → force-stop → 轮询新 uid → 清空目标（保留 lib/cache/code_cache/no_backup/.ota）
→ 解包（跳过 cache/code_cache/no_backup）→ `chown -hR 新uid:gid` → SELinux 重打标签
→ 属主校验。全程打印每一步结果，任何一步失败都会明确告警而不是静默继续。

#### 其它

```bash
su -c './ozj-appdata info --pkg com.tencent.mm'        # 看目录、uid、gid、context
su -c './ozj-appdata list --in /sdcard/ozj_backup/xxx' # 看备份内容
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
