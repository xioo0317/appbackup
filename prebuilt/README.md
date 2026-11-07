# prebuilt/

从上游 [Android-DataBackup](https://github.com/XayahSuSuSu/Android-DataBackup) 的
`source/app/src/arm64-v8a/assets/bin.zip` **原样提取**的二进制，用于保证归档字节与上游一致。

| 文件 | 大小 | 说明 |
|------|------|------|
| `tar`  | 1,110,776 | GNU tar，AArch64 静态链接 |
| `zstd` | 1,417,448 | zstd 1.5.6，AArch64 静态链接 |

本项目只发布 **Android arm64-v8a** 一个 ABI（其它 ABI 的构建会在 CMake 配置阶段直接报错）。

未做任何修改，SHA-256 与上游 `bin.zip` 内一致：

```
882639ac310a7eb4052c68c21cea02633307700f9cc8c7c469c2dd18d734a112  tar
2388211eb3960070c6b4528f68f7129a9ef5d165a0fef0113ac59e723006f4ca  zstd
```

## 为什么必须自带

归档字节由「**哪个 tar 实现**」决定，而不是由调用它的语言决定。

- Android 系统 `tar` 是 toybox 精简版：不认 `--totals`，头部字段（uid/gid 名、pax 记录、
  长名策略）也与 GNU tar 不同；
- 上游之所以能和自己的备份字节一致，正是因为它**从不调用系统 tar**——`bin.zip` 解压到
  私有目录后 `export PATH=<binDir>:$PATH`（见其 `BaseUtil.EnvInitializer`），调到的永远是
  这份 GNU tar。

本项目把它们**嵌进可执行文件**，首次运行时释放到磁盘（可执行文件同级的 `bin/`，或
`/data/local/tmp/.appbackup-bin`），分发仍然是单文件。

嵌入方式：CMake 配置期生成 `embed_<tool>.S`，用汇编指令 `.incbin` 把二进制原样放进
`.rodata`，符号 `appbackup_<tool>_begin/end`。

> 早期实现用 `ld -r -b binary`，在 NDK 上会直接失败——NDK 的 `ld` 是 `ld.lld`，不支持
> bfd 的 `-b/--format`，报 `target emulation unknown: -m or at least one .o file required`。
> `.incbin` 由 clang 内置汇编器支持，不依赖 binutils。

若释放失败（ABI 不符 / noexec），会回退系统 tar，并由 `toolchain` 自动去掉不被支持的
选项——功能可用，但字节不再保证与上游一致。

## 许可

- GNU tar：GPL-3.0-or-later
- zstd：BSD-3-Clause / GPL-2.0 双许可

源码见上游仓库、<https://www.gnu.org/software/tar/>、<https://github.com/facebook/zstd>。
本项目需以 GPL 兼容许可分发。
