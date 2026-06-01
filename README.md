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

默认只会生成 wrapper：

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

默认 `make` 和 `make install` 不会构建或安装 `randcore-child-balancer` 服务。服务需要显式构建和安装，见下文。

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

## wrapper 配置项

- `RANDCORE_STATE_DIR`：状态文件和锁文件目录，默认 `/tmp`。
- `RANDCORE_SET_AI_THREAD`：HMP proc 文件路径，默认 `/proc/set_ai_thread`。
- `RANDCORE_STRICT=1`：状态文件/锁失败，或选择 A100 但写入 `/proc/set_ai_thread` 失败时直接退出。
- `RANDCORE_QUIET=1`：隐藏状态文件或 `/proc/set_ai_thread` 失败时的警告。
- `RANDCORE_LOG=1`：打印每次路由决策和当前计数。

## 子进程均衡服务

如果无法通过 `CC/CXX` 指定 wrapper，可以使用 `randcore-child-balancer` 监控已有构建进程树。服务从指定父 PID 的后代中查找匹配的编译器进程，并按 X100/A100 当前管理数量做均衡调度。

注意：X100 和 A100 核心的 V 扩展 vlen 不同，不应该对会使用 V 扩展的程序使用这个服务。服务是在进程已经启动后从外部发现并设置 HMP 标志，可能晚于程序或其子进程执行 V 指令。`randcore-*` wrapper 是在真实程序启动前设置亲和性，再 `exec` 到目标程序，因此可以放心用于这类程序。

重要规则：

- `RANDCORE_PARENT_PIDS` 指定的是根 PID，根 PID 自身不会被调度，服务只扫描它的后代。
- 当某个后代命中 `RANDCORE_MATCH_NAMES` 并纳入管理后，服务会停止继续把这个进程的子树作为独立任务调度。例如 `make -> gcc -> ld` 中只管理 `gcc`，不会再单独管理 `ld`。
- 选择 A100 时，服务会把命中的进程和当次扫描快照里已经存在的整个子树一起写入 `/proc/set_ai_thread`，避免 `gcc/c++` 在扫描间隔内已经 fork 出 `cc1/cc1plus` 时漏标。
- 未命中的中间进程不会阻断扫描。例如 `make -> sh -> gcc` 仍会管理 `gcc`。
- 选择 A100 时写 `/proc/set_ai_thread`。选择 X100 时保持默认 Regular/X100 行为，因为当前内核只暴露用户态设置 AI 线程的接口。

构建 daemon：

```sh
make randcore-child-balancer
```

安装服务：

```sh
sudo make install-child-balancer
sudo cp /etc/randcore-child-balancer.env.example /etc/randcore-child-balancer.env
```

编辑 `/etc/randcore-child-balancer.env`：

```sh
RANDCORE_PARENT_PIDS=12345
RANDCORE_MATCH_NAMES=*gcc,*g++,gcc,g++,cc,c++,clang,clang++
RANDCORE_SCAN_INTERVAL_MS=100
RANDCORE_LOG=1
```

启用服务：

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now randcore-child-balancer.service
sudo journalctl -u randcore-child-balancer.service -f
```

服务配置项：

- `RANDCORE_PARENT_PIDS`：逗号、分号或空白分隔的根 PID 列表。
- `RANDCORE_MATCH_NAMES`：逗号、分号或空白分隔的 `fnmatch` 模式，默认 `*gcc,*g++,gcc,g++,cc,c++,clang,clang++`。
- `RANDCORE_SCAN_INTERVAL_MS`：扫描间隔，默认 `100`，允许范围 `10` 到 `60000`。
- `RANDCORE_SET_AI_THREAD`：HMP proc 文件路径，默认 `/proc/set_ai_thread`。
- `RANDCORE_STRICT=1`：写 `/proc/set_ai_thread` 失败时退出服务。
- `RANDCORE_QUIET=1`：隐藏警告。
- `RANDCORE_LOG=1`：打印每次纳入管理的路由决策。
- `RANDCORE_DRY_RUN=1`：只打印决策，不写 `/proc/set_ai_thread`。
- `RANDCORE_ONESHOT=1`：扫描一次后退出，主要用于验证。

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

检查子进程均衡服务的 dry-run 行为：

```sh
bash -c '
  exec -a gcc bash -c "exec -a ld sleep 10 & wait" &
  fake=$!
  sleep 0.2
  RANDCORE_PARENT_PIDS=$$ RANDCORE_MATCH_NAMES=gcc,ld RANDCORE_DRY_RUN=1 RANDCORE_LOG=1 \
    ./randcore-child-balancer --once
  kill "$fake" 2>/dev/null || true
  wait "$fake" 2>/dev/null || true
'
```

在真实构建中，日志会显示纳入管理的顶层匹配进程：

```text
randcore-child-balancer: tie dry-run X100 count=0 A100 count=0 -> A100 pid=12346 name=gcc
```

如果 `gcc` 已被纳入管理，它启动的 `ld` 不应再次出现在服务日志中。

## 许可证

WTFPL，见 [LICENSE](LICENSE)。
