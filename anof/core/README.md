# 公共基础类型

core 放置各层都会用到的小型类型，例如 ID、数量和操作结果。这样 HTTP 输入、业务代码与数据库读取可以使用相同的值校验，而不必各自解释同一个字段。

[Id.h](Id.h) 用不同标签区分用户、空间等 ID，避免把一种 ID 误传给另一种接口；解析时检查 UUID 形式并统一大小写。[Quantity.h](Quantity.h) 和 [Page.h](Page.h) 处理数量与分页边界，[Result.h](Result.h) 和 [Error.h](Error.h) 用于返回成功值或错误。[Clock.h](Clock.h) 提供可替换的时钟，方便业务代码使用真实时间，也方便测试控制时间。

这些类型只检查值本身。一个合法的节点 ID 不代表节点存在，更不代表调用者有权访问它。业务规则放在 [domain](../domain/README.md)，外部协议转换放在对应适配器。

值边界的例子可看 [Core_unittest.cc](tests/Core_unittest.cc)，测试运行方法见 [测试说明](../../tests/README.md)。
