# appbackup

Android 应用数据 **打包 / 恢复** 命令行工具（C++17，需 root）。

参照 [Android-DataBackup-custom](https://github.com/boluo4169-commits/Android-DataBackup-custom)（v3.15.2）的
备份/恢复流程实现，但去掉 Kotlin/Gradle 依赖，做成单个可执行文件。
**不手写 tar**：把上游 `assets/bin.zip` 里的 GNU tar + zstd 按 ABI 内嵌进可执行文件，运行时释放后调用，
因此归档字节与上游同源（详见 [prebuilt/README.md](prebuilt/README.md)）。

**与 DataBackup 双向兼容**：备份直接产出它认识的目录结构（`apps/<应用名>_<包名>/user_<uid>/`，含
`package_restore_config.json`、`.md5` 与 `.manifest`），它能识别、扫描并恢复；
反过来，本工具也能直接恢复它生成的备份目录。

> **v2.1.0：备份逻辑按上游重写，废弃自有格式**。只产出 DataBackup 的 `apps/<应用名>_<包名>/user_<uid>/` 结构，
> 每个归档配 `.md5` + `.manifest` 三件套，`package_restore_config.json` 按 Gson 的 PackageEntity 写全，
> 增量指纹与上游同口径；`meta.json` 与 `--format` 不再生成，`restore` 对旧备份保持只读兼容。
>
> v2.0.0 起项目更名为 `appbackup`（原名 `ozj-appdata`），可执行文件 / CMake 工程名 / 命名空间 / CI 产物名统一。

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
├── <每个归档>.md5                   # 恢复前校验完整性
└── <每个归档>.manifest              # 增量指纹：源清单哈希 + 打包参数
```

`preserveId` 是保留版本的毫秒时间戳；主备份（最新一份）用 `user_<uid>` 不带 `@`。
`<label>_` 前缀**默认开启**，与上游 `PackageEntity.archivesRelativeDir` 一致：
`sanitizedLabel = label.replace(Regex("[^\\u4e00-\\u9fa5A-Za-z0-9_]"), "").trim('_')`
（只保留中文/英文/数字/下划线，空格和斜杠直接删除）。取不到应用名、或显式 `--label plain` 时
退回纯包名目录（legacy 格式）；`list` / `restore` 对两种目录都能识别。
CLI 取 label 依次尝试 `/data/system/packages.xml` 的 `label=` 属性、`aapt dump badging <base.apk>`
的 `application-label:`，都没有时可用 `--label <应用名>` 显式指定。

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

### 应用已卸载时：APK 归档必须能被找到

`restoreData` 依赖 uid，所以应用卸载后**必须先装 APK**。本工具按下面顺序定位 APK 归档：

1. 版本目录里的 `apk.tar.zst`（兼容 `.tar.gz` / `.tar` / `.tar.lz4`）；
2. 找不到时，从同应用目录下其它 `user_<uid>@<preserveId>` 版本借用一份
   —— 上游 preserve 会把旧主备份归档到那里，APK 常常只留在旧版本里；
3. 仍找不到时，报错会打印**查过哪些文件名、目录里实际有哪些 apk 开头的文件**。

常见误判：`apk.tar.zst` 里明明有 `./base.apk`，却仍报"没有可用的 apk 归档"。
原因是**文件不在 `--in` 指向的那个版本目录里**（例如只把数据归档复制到了
`/sdcard/DataBackup/apps/<pkg>/user_0`，把体积最大的 APK 和
`package_restore_config.json` 留在了原备份目录）。这种情况下日志里的 `ct=` 会是空的
—— 因为连 config 都没读到。把 APK 归档放回该目录，或让 `--in` 指向完整备份目录即可。

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

第 1、2、3、7 条由 `restore_policy.h` 的规则函数集中实现（`should_restore_entry` /
`external_gid`），恢复前统一过一遍，不靠人工比对；主机构建（`-DAPP_HOST_CHECK=ON`）可编译验证。

---

## 三、本工具

### 与上游的差异

| 项 | DataBackup | appbackup |
| --- | --- | --- |
| 形态 | Android App（Kotlin + Compose） | 单个 C++ 可执行文件（arm64-v8a，内嵌 tar + zstd） |
| tar | 调用自带 GNU tar | **同一份** GNU tar（从上游 `assets/bin.zip` 提取后内嵌） |
| 压缩 | zstd / lz4 / tar | zstd / tar（DataBackup 不认 gzip，所以不产出 gzip） |
| 入口 | GUI | 命令行 |
| 备份目录 | `apps/<应用名>_<包名>/user_<uid>/` + `@<ts>` 保护版本 | **完全一致** |
| 归档三件套 | `<type>.tar.zst` + `.md5` + `.manifest` | **完全一致** |
| 增量 | 源清单哈希比对 | 同款 `find -printf \| sort \| sha256sum` 口径 |
| 额外 | — | 路径穿越防护、硬链接去重、主备份就地更新先写 `.part` 再 rename |

**没有自有格式**：备份只产出 DataBackup 的目录结构，不存在 `meta.json`、也不再有 `--format` 开关。
反过来，`restore` 仍能读旧版本生成的 `meta.json` 备份（只读兼容，便于把老备份取回来）。

**不手写 tar**：归档字节由「哪个 tar 实现」决定，手写永远对不齐 pax 头、uname 规则、
`--totals` 行为。所以直接内嵌上游那份 GNU tar（`prebuilt/arm64-v8a/`），首次运行释放到
磁盘后调用，和上游 App 走的是同一条命令、同一份二进制。

### 构建

只产 **Android arm64-v8a** 一个 ABI（其它 ABI 在 configure 阶段直接报错）。

```bash
# 主机编译检查（内嵌的是 aarch64 二进制，只验编译，不能跑备份）
sudo apt install -y cmake g++ zlib1g-dev
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release -DAPP_HOST_CHECK=ON
cmake --build build-host -j
./build-host/appbackup --version

# Android（NDK r27+）
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

CI 只产出一个 `appbackup-arm64-v8a.zip`。

### 用法

```
appbackup backup  --pkg <包名> [--user 0] [--types user,user_de,data,obb,media,apk]
                    [--out DIR] [--compress zstd|none] [--level N] [--threads N]
                    [--label auto|plain|<应用名>] [--preserve] [--no-incremental]
                    [--kill none|am|kill]
appbackup restore --in <备份目录> [--pkg <包名>] [--user 0] [--types ...]
                    [--clean] [--install-apk] [--no-selinux] [--no-verify]
                    [--no-checksum] [--force-checksum]
appbackup list    --in <备份目录> [--pkg <包名>]
appbackup info    --pkg <包名> [--user 0]
```

#### 备份

```bash
su -c './appbackup backup --pkg com.tencent.mm --out /sdcard/DataBackup --kill am'
su -c './appbackup backup --pkg com.tencent.mm --out /sdcard/DataBackup --preserve --level 3'
```

产出目录与 DataBackup 逐字节同构：

```
/sdcard/DataBackup/
└── apps/微信_com.tencent.mm/         # 目录名 = sanitizedLabel + "_" + 包名
    ├── user_0/                       # 主备份（不带 @；--preserve 时旧目录变 user_0@<毫秒时间戳>）
    │   ├── apk.tar.zst               # 顶层就是 ./base.apk、./split_config.*.apk
    │   ├── apk.tar.zst.md5           # 32 位十六进制，DataBackup 恢复前校验
    │   ├── apk.tar.zst.manifest      # 增量指纹：v/hash/ct/level/threads/follow/excl
    │   ├── user.tar.zst
    │   ├── user.tar.zst.md5
    │   ├── user.tar.zst.manifest
    │   ├── user_de.tar.zst(.md5/.manifest)
    │   ├── data.tar.zst(.md5/.manifest)
    │   ├── obb.tar.zst(.md5/.manifest)
    │   ├── media.tar.zst(.md5/.manifest)
    │   └── package_restore_config.json   # Gson 的 PackageEntity，字段与顺序同上游
    └── user_0@1794056351000/         # 上一份（--preserve 时）
```

- **源不存在的类型不产出归档**；`dataStates` 里对应项仍写 `Selected`（上游默认 8 项全选，
  状态含义是「用户勾选」而非「源存在」）。上游扫描恢复时会按实际归档重建状态，不会去找不存在的文件。
- **`.manifest` 的 hash** 是 `find [-L] <src> [-maxdepth 1] <prune> -type f -printf '%P|%s|%T@\n' | LC_ALL=C sort | sha256sum`。
  本工具不走 shell（Android 的 toybox `find` 未必支持 `-printf`），在 C++ 里按同一口径遍历、排序、SHA-256，
  与 GNU find 管道**逐字节一致**（自测里直接跟 `find | sort | sha256sum` 对比过）。
- **增量**：归档旁有 `.manifest`、指纹与打包参数（ct/level/threads/follow/excl）都没变、归档还在 → 该类型跳过重打。
  `--no-incremental` 强制全量；manifest 缺失/解析失败/读源失败一律按「已变化」全量，不会漏打。
- **就地更新安全**：先写 `<type>.tar.zst.part`，成功后才 rename 覆盖，中途失败不会截断上一份好归档。

#### 旁挂文件与配置文件的生成原理

以 `obb.tar.zst.manifest` 为例，字段全部来自上游 `IncrementalBackupUtil.serialize`：

```
v=1                  文件格式版本（MANIFEST_VERSION）
hash=<64位小写hex>    源目录文件清单的 SHA-256（不是归档内容的哈希；归档的哈希是 .md5）
ct=ZSTD              CompressionType.name（TAR / ZSTD / LZ4）
level=1              readCompressionLevel()，默认 1
threads=2            readCompressionThreads()，默认 2 → zstd 的 -T<threads>
follow=false         readFollowSymlinks()，对应 tar -h
excl=...             排除清单，用 | 连接；与 tar --exclude= 同一份列表
```

- `hash` 的算法：`find <src> [-maxdepth 1] [-path <prune> -prune -o] -type f
  -printf '%P|%s|%T@\n' | LC_ALL=C sort | sha256sum`。只 stat 不读文件内容，
  清单是「相对路径|大小|mtime」，排序后哈希。**空目录/全被排除 → 空清单 → 空串的 SHA-256
  `e3b0c442...855`**（模板里 obb 的 hash 就是这个值）。剪枝只对常量目录名做（与 --exclude 的目录项一一对应），
  glob 类（Backup_*、.trashed-*）不进剪枝：walk 集合是 tar 集合的超集，宁可白打不可漏打。
- `excl` 与 tar 同源（`buildExclusionAndPrune`）：
  - USER / USER_DE：`<pkg>/.ota|<pkg>/cache|<pkg>/lib|<pkg>/code_cache|<pkg>/no_backup`
  - DATA / OBB / MEDIA：`<pkg>/cache|Backup_*|.trashed-*|.pending-*|.Trashed-*|.Pending-*`
    （后四项是 `PathUtil.TrashedFilePatterns`，外部存储的回收站/待安装临时文件）
  - APK：空（tar 只收顶层 `*.apk`）
- `threads` 是**设置项**不是设备核数：默认 2（`readCompressionThreads` 的 defValue），用户可在设置里改。
  它同时进 zstd 命令（`-T<threads>`）和 manifest 参数指纹。例外：用户改设置、不同版本默认值、
  或关闭自定义线程。实测在管道 stdin + level 1 下，`-T0/-T1/-T2` 与带/不带 `--ultra --priority=rt`
  生成的 `.tar.zst` **字节完全相同**，所以它不改变归档内容，只影响压缩耗时/发热与增量判定。
  CLI 对应 `--threads <n>`（默认 2）。
- `.md5` = 压缩后归档文件的 MD5（32 位 hex），恢复前校验完整性；与 `hash` 完全不同。

`package_restore_config.json` 是上游 `PackageEntity` 的 Gson 序列化（`GsonBuilder().setPrettyPrinting()`），
备份成功后由 `AbstractBackupService` 写：`restoreEntity = p.copy(id = 旧 RESTORE 记录 id ?: 0,
indexInfo = p.indexInfo.copy(opType = RESTORE, cloud, backupDir), extraInfo = p.extraInfo.copy(activated = false))`。

| 字段块 | 来源 |
| --- | --- |
| `id` | Room 数据库 `@PrimaryKey(autoGenerate)`；首次备份无旧记录 → 0（本工具写 0） |
| `indexInfo` | 任务/设置：opType 固定 RESTORE、packageName、userId、compressionType、preserveId=0、cloud、backupDir |
| `packageInfo` | `PackageManager.getPackageInfo`：label、versionName、versionCode、flags、firstInstallTime、lastUpdateTime |
| `extraInfo` | uid、permissions（`requestedPermissions` + granted 标志 + AppOps op/mode，无记录时 mode 默认 `MODE_IGNORED=1`）、ssaid（settings_ssaid.xml）、lastBackupTime=当前时间戳、blocked/activated/firstUpdated/enabled |
| `dataStates` | `PackageDataStates` 默认 8 项全 Selected = 用户勾选状态 |
| `storageStats` | `StorageStats` 系统 API；备份时通常全 0，恢复时才回填 |
| `dataStats` | `calculateSize(src)` 源目录原始大小；**USER 例外**：上游刻意跳过（海量小文件遍历要几十秒），用归档大小回填 → 与 displayStats 相等 |
| `displayStats` | `calculateSize(dst)` 归档（压缩后）文件大小；实测与 `.tar.zst` 字节数逐个精确相等 |

上游恢复时会重新扫描目录并覆盖 `indexInfo.packageName/userId/preserveId`、按实际归档重建 `dataStates`，
所以 JSON 的键顺序、缩进、id 都不影响识别（Gson 按名字读）。CLI 与 App 的差别只在「怎么采集」：
App 用 Java API（`PackageManager`、`AppOpsManager`、`getApplicationLabel`），CLI 用
`dumpsys package` / `cmd package dump` / `aapt dump badging` / `appops get` / 直接读 `settings_ssaid.xml`。

#### 恢复（换机）

```bash
# 版本目录 / 应用目录 / 备份根目录三种都行，自动下探到最新一份
su -c './appbackup restore --in /sdcard/DataBackup --pkg com.tencent.mm --install-apk --clean'
su -c './appbackup restore --in /sdcard/DataBackup/apps/com.tencent.mm --install-apk --clean'
su -c './appbackup restore --in /sdcard/DataBackup/apps/com.tencent.mm/user_0 --install-apk --clean'

# 旧版本自己格式的备份（meta.json）仍可直接恢复
su -c './appbackup restore --in /sdcard/appbackup/com.tencent.mm_20260101_120000 --install-apk --clean'
```

流程：force-stop → 装 APK（split 走会话）→ 轮询新 uid → **先记目标目录 context 再决定是否 mkdir**
→ 清空目标（保留 lib/cache/code_cache/no_backup/.ota）→ 校验 md5 → 解包到**父目录**
（跳过 .ota/cache/lib/code_cache/no_backup，外部存储再加 Backup_*/trashed）→ `chown -hR 新uid:gid`
→ SELinux 重打标签 → 属主二次校验。全程打印每一步结果，任何一步失败都会明确告警而不是静默继续。

`--in` 指向备份根目录时若有多份应用备份，必须用 `--pkg` 指定，否则会直接报错列出来，不会猜。

#### 其它

```bash
su -c './appbackup info --pkg com.tencent.mm'                    # 看目录、uid、gid、context
su -c './appbackup list --in /sdcard/DataBackup/apps/com.tencent.mm/user_0'
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
