export type Route = 'login' | 'register' | 'account' | 'password' | 'files' | 'admin' | 'transfers';
export function parseRoute(hash: string): Route | null {
  const match = /^#\/(login|register|account|password|files|admin|transfers)$/u.exec(hash);
  return match ? match[1] as Route : hash === '' || hash === '#' ? 'account' : null;
}
export function navigate(route: Route): void {
  if (location.hash !== `#/${route}`) location.hash = `#/${route}`;
}
