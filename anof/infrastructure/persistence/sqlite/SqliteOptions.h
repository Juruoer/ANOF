#pragma once

namespace anof::infrastructure::sqlite {

struct SqliteOptions {
    // 限制为 0～5000 ms；0 表示遇到竞争立即返回。
    int busyTimeoutMs = 100;
};

}  // namespace anof::infrastructure::sqlite
