import {TransferView} from './transfer/views.ts';
import {AdminView} from './admin/views.ts';
import {AdminApi} from './admin/api.ts';
import {DriveView} from './drive/views.ts';
import {DriveApi} from './drive/api.ts';
import {ApiClient} from './api/client.ts';
import {IdentityStore} from './auth/store.ts';
import {accountView} from './auth/views.ts';
import {button, element} from './ui/dom.ts';
import {navigate, parseRoute} from './ui/router.ts';
import {describeFailure} from './api/errors.ts';

const root = document.getElementById('app');
if (!root) throw new Error('Application root is missing');
const title = element('h1', 'ANOF');
title.id = 'app-title';
const nav = element('nav');
nav.setAttribute('aria-label', '主导航');
const notice = element('p');
notice.setAttribute('role', 'status');
notice.setAttribute('aria-live', 'polite');
const content = element('div');
root.replaceChildren(title, nav, notice, content);

if (location.protocol !== 'https:') {
    notice.textContent = '请通过配置的同源 HTTPS 入口访问 ANOF。';
} else {
    const api = new ApiClient(location.origin);
    const store = new IdentityStore(api);
    const showError = (message: string) => {
        notice.textContent = message;
    };
    let transfer: TransferView | null = null;
    let transferBinding: string | null = null;
    let admin: AdminView | null = null;
    let adminBinding: string | null = null;
    let drive: DriveView | null = null;
    let driveBinding: string | null = null;

    function render(): void {
        const state = store.state;
        if (transfer && (state.phase !== 'authenticated' || state.busy || state.binding !== transferBinding || parseRoute(location.hash) !== 'transfers')) {
            transfer.destroy();
            transfer = null;
            transferBinding = null;
        }
        if (admin && (state.phase !== 'authenticated' || state.busy || state.user?.role !== 'administrator' || state.binding !== adminBinding || parseRoute(location.hash) !== 'admin')) {
            admin.destroy();
            admin = null;
            adminBinding = null;
        }
        if (drive && (state.phase !== 'authenticated' || state.busy || state.binding !== driveBinding || parseRoute(location.hash) !== 'files')) {
            drive.destroy();
            drive = null;
            driveBinding = null;
        }
        nav.replaceChildren();
        notice.textContent = state.notice;
        if (state.busy) {
            content.replaceChildren(element('p', '正在处理，请勿重复提交。'));
            content.setAttribute('aria-busy', 'true');
            return;
        }
        content.removeAttribute('aria-busy');
        if (state.phase === 'checking') {
            content.replaceChildren(element('p', '正在核对登录状态…'));
            return;
        }
        if (state.phase === 'error') {
            content.replaceChildren(button('重新核对身份', () => {
                void store.refresh();
            }));
            return;
        }
        const authenticated = state.phase === 'authenticated';
        for (const [label, route] of authenticated ? [['账户资料', 'account'], ['文件管理', 'files'], ['传输管理', 'transfers'], ['修改密码', 'password']] as const : [['登录', 'login'], ['邀请注册', 'register']] as const) {
            const link = element('a', label);
            link.href = `#/${route}`;
            nav.append(link);
        }
        if (authenticated && state.user?.role === 'administrator') {
            const link = element('a', '管理员管理');
            link.href = '#/admin';
            nav.append(link);
        }
        nav.append(button(authenticated ? '登出' : '再次发起登出', () => {
            void store.logout().catch((error: unknown) => showError(describeFailure(error)));
        }));
        if (!authenticated) nav.append(button('重新核对身份', () => {
            void store.refresh();
        }));
        let route = parseRoute(location.hash);
        if (route === null) {
            content.replaceChildren(element('p', '页面不存在。'), button('返回账户入口', () => navigate(authenticated ? 'account' : 'login')));
            return;
        }
        if (!authenticated && (route === 'account' || route === 'password' || route === 'files' || route === 'admin' || route === 'transfers')) route = 'login';
        if (authenticated && (route === 'login' || route === 'register')) route = 'account';
        if (route === 'transfers' && state.user) {
            if (!transfer) {
                transfer = new TransferView(api, state.user);
                transferBinding = state.binding;
            }
            content.replaceChildren(transfer.root);
        } else if (route === 'files' && state.user) {
            if (!drive) {
                drive = new DriveView(new DriveApi(api, state.user));
                driveBinding = state.binding;
            }
            content.replaceChildren(drive.root);
        } else if (route === 'admin' && state.user) {
            if (state.user.role !== 'administrator') content.replaceChildren(element('p', '此入口仅供管理员使用。'));
            else {
                if (!admin) {
                    admin = new AdminView(new AdminApi(api, state.user));
                    adminBinding = state.binding;
                }
                content.replaceChildren(admin.root);
            }
        } else if (route !== 'files' && route !== 'admin' && route !== 'transfers') content.replaceChildren(accountView(store, route, showError));
    }

    store.subscribe(render);
    window.addEventListener('hashchange', () => {
        if (store.state.busy) {
            render();
            return;
        }
        api.cancelReads();
        if (store.state.phase === 'checking') void store.refreshAutomatically();
        render();
    });
    window.addEventListener('pagehide', () => store.invalidate(store.state.notice));
    window.addEventListener('pageshow', (event) => {
        if (event.persisted) void store.refreshAutomatically();
    });
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') void store.refreshAutomatically();
    });
    render();
    void store.refreshAutomatically();
}
