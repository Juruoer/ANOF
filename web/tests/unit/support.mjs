export const ID = '10000000-0000-4000-8000-000000000001';
export const INSTANCE = '20000000-0000-4000-8000-000000000001';
export const profile = { id: ID, login: 'alice', display_name: 'Alice', role: 'member', enabled: true,
  revision: '9007199254740993', created_at: '2026-10-27T00:00:00.000001Z', updated_at: '2026-10-27T00:00:00.000002Z' };
export const tag = `"user-${ID}-${profile.revision}"`;
export const json = (value, status = 200, headers = {}) => new Response(JSON.stringify(value), {
  status, headers: { 'Content-Type': 'application/json', ...headers },
});
export const fault = (status, code) => json({ error: { code, message: '<img src=x onerror=alert(1)>', request_id: ID, details: {} } }, status);
export const me = (data = profile) => json({ ...data, instance_id: INSTANCE }, 200, { etag: `"user-${data.id}-${data.revision}"` });
