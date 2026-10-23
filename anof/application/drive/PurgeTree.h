#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
namespace anof {
// 调用方负责授权/到期与条件校验；复用同一事务，不自行提交。
Result<void> purgeTree(DriveTransaction&, const TrashEntry&, UtcTime, RequestId, DriveActor);
}
