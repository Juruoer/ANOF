#pragma once
#include <anof/application/ports/InstancePreparation.h>
#include <anof/domain/maintenance/MaintenanceValues.h>
#include <anof/domain/transfer/Upload.h>
namespace anof::infrastructure::sqlite {
// 文件适配器在取证前调用；返回前关闭读事务和连接，不跨文件 I/O。
Result<void> validateMaintenanceUploadFacts(const Upload&, const ResumeUpload&, const MaintenanceActionEvidence&);
Result<Upload> readMaintenanceUpload(const InstanceLease&, UploadId);
}  // namespace anof::infrastructure::sqlite
