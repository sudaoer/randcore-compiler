# randcore 编译器转发器

`randcore-gcc`、`randcore-g++`、`randcore-clang`、`randcore-clang++` 等是用于 K3 的编译器 wrapper。它们会记录由 randcore 启动且仍在运行的顶层编译器进程数量，并优先把新任务分配到当前任务数较少的 CPU 簇：

- X100：CPU0-7
- A100：CPU8-15

两边任务数相等时，wrapper 会在 X100/A100 之间轮转；首次相等默认选择 A100，之后交替选择。

实现依赖 K3 HMP 机制：task 写入 `/proc/set_ai_thread` 后，AI 标记会跨 `exec()` 保留。因此 wrapper 选择 A100 时，会在子进程中先写 `/proc/set_ai_thread`，再 `exec` 到真实编译器。真实编译器及其后续 fork 出来的 `cc1`、`as`、`ld` 等子进程会继承该标记。

wrapper 的目标编译器由文件名决定：`randcore-xxx` 会自动执行 `PATH` 中的 `xxx`。例如：

- `randcore-gcc` 执行 `PATH` 中的 `gcc`
- `randcore-g++` 执行 `PATH` 中的 `g++`
- `randcore-clang` 执行 `PATH` 中的 `clang`
- `randcore-riscv64-linux-gnu-gcc` 执行 `PATH` 中的 `riscv64-linux-gnu-gcc`

## 构建

源码是 `compiler-wrapper.cpp`，默认使用 `g++` 和 C++17 编译：

```sh
make
```

默认会生成：

```text
randcore-gcc
randcore-g++
randcore-clang
randcore-clang++
randcore-cc
randcore-c++
```

如果需要其它名字，可以直接构建对应目标：

```sh
make randcore-riscv64-linux-gnu-gcc
```

## 使用

```sh
CC=/path/to/randcore-gcc CXX=/path/to/randcore-g++ make -j16
```

使用 clang：

```sh
CC=/path/to/randcore-clang CXX=/path/to/randcore-clang++ make -j16
```

wrapper 默认把状态写到 `/tmp/randcore-compiler-$UID.state`，锁文件为 `/tmp/randcore-compiler-$UID.lock`。状态文件只记录 randcore 启动的顶层真实编译器 PID 和下一次相等时应选择的 CPU 簇；读取状态时会清理已经退出或 PID 已复用的记录，不统计编译器后续 fork 出来的子孙进程。

如果状态文件不可用，默认会打印警告并直接执行真实编译器，此时不记录任务状态。若选择 A100 但写入 `/proc/set_ai_thread` 失败，默认会打印警告并回退到 X100/default 路径继续执行。

## 配置项

- `RANDCORE_STATE_DIR`：状态文件和锁文件目录，默认 `/tmp`。
- `RANDCORE_SET_AI_THREAD`：HMP proc 文件路径，默认 `/proc/set_ai_thread`。
- `RANDCORE_STRICT=1`：状态文件/锁失败，或选择 A100 但写入 `/proc/set_ai_thread` 失败时直接退出。
- `RANDCORE_QUIET=1`：隐藏状态文件或 `/proc/set_ai_thread` 失败时的警告。
- `RANDCORE_LOG=1`：打印每次路由决策和当前计数。

## 验证

在 K3 上可以用下面的命令检查 wrapper 是否会平衡分配：

```sh
make randcore-sh
for i in 1 2 3 4; do
  RANDCORE_LOG=1 ./randcore-sh -c \
    'grep Cpus_allowed_list /proc/$$/status; sleep 1' &
done
wait
```

期望结果：

- 日志中的 `X100 count=... A100 count=... -> ...` 会优先选择任务数较少的一边。
- X100 任务的 `Cpus_allowed_list` 通常是 `0-7`。
- A100 任务的 `Cpus_allowed_list` 通常是 `8-15`。
