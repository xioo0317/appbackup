# prebuilt/

从上游 [Android-DataBackup](https://github.com/XayahSuSuSu/Android-DataBackup) 的
`source/app/src/<abi>/assets/bin.zip` **原样提取**的二进制，用于保证归档字节与上游一致。
四个 ABI 齐全，构建时按 `ANDROID_ABI` 自动选用同 ABI 的一份。

| ABI | tar | zstd |
|-----|-----|------|
| `arm64-v8a`   | 1,110,776 (AArch64 静态) | 1,417,448 (zstd 1.5.6, AArch64 静态) |
| `armeabi-v7a` |   764,204 (ARM 静态)     | 1,173,372 (zstd 1.5.6, ARM 静态)     |
| `x86_64`      | 1,224,872 (x86-64 静态)  | 1,712,728 (zstd 1.5.6, x86-64 静态)  |
| `x86`         | 1,262,544 (i386 静态)    | 1,795,264 (zstd 1.5.6, i386 静态)    |

未做任何修改，SHA-256 与上游 `bin.zip` 内一致：

```
882639ac310a7eb4052c68c21cea02633307700f9cc8c7c469c2dd18d734a112  arm64-v8a/tar
2388211eb3960070c6b4528f68f7129a9ef5d165a0fef0113ac59e723006f4ca  arm64-v8a/zstd
fbd100eac039d51ade8fbf93e5381ee984eec8de4bbdd465e67b1ef6780370ed  armeabi-v7a/tar
9a8c53503f2025c07c42878d4932cb91c5c82d3849cf235b56214b41102ea495  armeabi-v7a/zstd
ba2b7e46eeee675ee16ed11336fe6243bdf41e03f75bb321e05cf50899f5d5da  x86_64/tar
4a808483bbe7d7c8c81857f8f253ffbd523b0249c03cb2551f63fb4084f187d5  x86_64/zstd
ffafc79b434149bf2e9308269e0cca49865627d3903616ec3eb2c8d211089322  x86/tar
302e9a6b6f1acf8edf4710656c43e179c0308ab20346701d263baf68abdc1d5b  x86/zstd
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
> `.incbin` 由 clang 内置汇编器支持，四个 ABI 通用，且不依赖 binutils。

若释放失败（ABI 不符 / noexec），会回退系统 tar，并由 `toolchain` 自动去掉不被支持的
选项——功能可用，但字节不再保证与上游一致。

## 许可

- GNU tar：GPL-3.0-or-later
- zstd：BSD-3-Clause / GPL-2.0 双许可

源码见上游仓库、<https://www.gnu.org/software/tar/>、<https://github.com/facebook/zstd>。
本项目需以 GPL 兼容许可分发。
