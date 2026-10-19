# 测试等待工具

[EventLoopWait.h](EventLoopWait.h) 提供测试专用的 `anof::test::waitFor(loop, predicate, seconds, file, line)`。
它只能在所属 loop 线程、loop 未运行时调用，不支持嵌套事件循环。默认轮询间隔为 1ms，等待上限为 3s；
调用方仍应配置有限 CTest 超时，因为 loop 内的阻塞回调无法由定时器抢占。

首次进入时检查一次 predicate，之后由轮询检查。第一个被观察到的终态固定结果：

- 轮询先观察到完成，之后同批次的超时回调不能覆盖成功。
- 超时先发生，之后的轮询不会再调用 predicate，也不能将结果改为成功。
- 其他代码退出 loop 而尚未观察到完成时返回 false。
- predicate 抛异常时，在退出 loop 并清理定时器后向调用方重新抛出。

这是回调观察顺序，不保证真实完成时刻严格早于某个物理截止。返回、诊断和超时回调均不重新执行 predicate，
因此 predicate 可以包含读取 socket 等副作用。初次已完成时直接返回；其他路径使用 RAII 取消本次定时器，
共享状态的终态检查防止迟到回调再次访问 predicate。调用者仍须保证其他业务回调和 loop 的寿命。

设置 `ANOF_WAIT_TRACE` 时记录双时钟诊断，格式见 [WaitTrace](../diagnostics/WaitTrace.h)。包装函数应转发
默认 `__builtin_FILE()`、`__builtin_LINE()` 参数，以保留真正等待调用点的位置，不要在外层重复执行谓词或记录同一组事件。

[回归测试](EventLoopWait_unittest.cc) 使用两个不同的过去单调期限，让真实 TimerQueue 在同一批次收集回调，
分别验证完成先发生和超时先发生。另覆盖有副作用的谓词、初始完成、普通超时、外部 quit、异常清理以及再次驱动
loop 时没有旧谓词调用。测试不修改系统时钟，不依赖 sleep 或随机调时。

```sh
cmake --build cmake-build-debug --target anof_event_loop_wait_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_event_loop_wait_unittest$' -j 1
```
