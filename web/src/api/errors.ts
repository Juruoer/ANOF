export type FailureKind = 'rejected' | 'unknown' | 'transport' | 'protocol' | 'cancelled' | 'stale' | 'local' | 'busy';

export class ApiError extends Error {
  readonly kind: FailureKind;
  readonly status: number;
  readonly code: string;
  constructor(kind: FailureKind, status = 0, code = '') {
    super(kind);
    this.name = 'ApiError';
    this.kind = kind;
    this.status = status;
    this.code = code;
  }
}

export const RECONCILE = '操作结果未知。请核对当前状态；未查到结果也不代表操作未生效。不要直接重复提交。';
export const CONFLICT = '资料已发生变化，已尝试刷新。请核对最新资料后重新决定，不会自动再次提交。';

export function describeFailure(error: unknown): string {
  if (!(error instanceof ApiError)) return '操作未完成，请重新检查当前状态。';
  if (error.kind === 'unknown') return RECONCILE;
  if (error.kind === 'cancelled' || error.kind === 'stale') return '读取已取消或页面已变化。';
  if (error.kind === 'busy') return '已有操作正在处理，请稍后再试。';
  if (error.kind === 'local') return '输入无效或超出限制，请检查后再提交。';
  if (error.kind === 'protocol') return '服务器响应不符合约定，无法确认当前状态。';
  if (error.kind === 'transport') return '读取失败，请检查连接后重新读取。';
  if (error.status === 412) return CONFLICT;
  const messages: Record<string, string> = {
    UNAUTHENTICATED: '身份验证失败或会话已失效，请重新登录。',
    SESSION_LIMIT_REACHED: '活动会话已达上限，请先结束其他会话。',
    RATE_LIMITED: '请求过于频繁，请稍后再试。',
    INVITE_UNAVAILABLE: '邀请不可用，请向管理员核实。',
    LOGIN_CONFLICT: '登录名已被使用，请选择其他登录名。',
    FORBIDDEN: '没有执行此操作的权限，或操作不符合当前账户限制。',
    NOT_FOUND: '资源不可见或已不存在，请刷新当前列表。',
    INVALID_NAME: '名称无效：请勿使用路径分隔符、控制字符或超过 255 字节。',
    INVALID_ARGUMENT: '输入或分页游标无效，请检查输入或返回列表首页。',
    QUOTA_EXCEEDED: '空间或公共贡献配额不足；回收站仍占用配额。',
    NAME_CONFLICT: '目标已有同名文件或目录，请明确选择另一个名称。',
    PARENT_NOT_DIRECTORY: '目标不是目录，请重新选择目标。',
    CROSS_SPACE_MOVE: '移动只能在同一空间内进行；单文件可以显式复制到其他空间。',
    DIRECTORY_CYCLE: '不能将目录移入自身或其后代。',
    DEPTH_LIMIT_EXCEEDED: '目标目录层级超出限制。',
    TREE_LIMIT_EXCEEDED: '此目录树超过单次操作的节点限制。',
    NODE_LIMIT_REACHED: '实例节点数已达上限。',
    RESTORE_TARGET_UNAVAILABLE: '原位置无法恢复，请明确选择其他目录。',
    QUERY_LIMIT_EXCEEDED: '查询超过服务端预算，请缩小搜索范围。',
    STORAGE_BUDGET_EXCEEDED: '内容预算低于当前已计费内容加预留，服务器已拒绝修改。请核对最新计量。',
    STORAGE_FULL: '服务端存储空间不足，操作未能完成。',
    TEMPORARILY_UNAVAILABLE: '服务暂不可用，请稍后核对当前状态。',
    REQUEST_ORIGIN_REJECTED: '请求来源被拒绝，请从配置的 HTTPS 地址访问。',
    CSRF_REQUIRED: '请求未通过同源安全检查。',
    COOKIE_MODE_DISABLED: '此服务未启用浏览器登录。',
    PRECONDITION_REQUIRED: '缺少资料版本，请重新读取资料。',
  };
  return messages[error.code] ?? '服务器拒绝了此操作，请检查输入与当前状态。';
}
