import type { IdentityStore } from './store.ts';
import { button, element, field } from '../ui/dom.ts';
import { describeFailure } from '../api/errors.ts';
import { navigate } from '../ui/router.ts';
import type { Route } from '../ui/router.ts';

export function accountView(store: IdentityStore, route: Exclude<Route, 'files' | 'admin' | 'transfers'>, showError: (message: string) => void): HTMLElement {
  const section = element('section');
  const form = element('form');
  form.method = 'post';
  let submit: () => Promise<void>;
  let label: string;
  const secrets: HTMLInputElement[] = [];
  if (route === 'login') {
    section.append(element('h2', '登录'));
    const login = field(form, '登录名', 'login', 'text', 'username'); login.maxLength = 32;
    const password = field(form, '密码', 'password', 'password', 'current-password'); secrets.push(password);
    label = '登录';
    submit = async () => {
      const value = password.value; password.value = '';
      await store.login(login.value, value);
      if (store.state.phase === 'authenticated') navigate('account');
    };
  } else if (route === 'register') {
    section.append(element('h2', '邀请注册'), element('p', '注册成功后需另行登录。'));
    const login = field(form, '登录名', 'login', 'text', 'username'); login.maxLength = 32;
    const display = field(form, '显示名称', 'display_name');
    const invite = field(form, '邀请码', 'invite_token', 'password'); invite.maxLength = 55;
    const password = field(form, '新密码', 'password', 'password', 'new-password');
    secrets.push(invite, password);
    label = '注册';
    submit = async () => {
      const token = invite.value, value = password.value; invite.value = ''; password.value = '';
      await store.register(login.value, display.value, value, token);
      if (store.state.notice.startsWith('注册成功')) navigate('login');
    };
  } else if (route === 'password') {
    section.append(element('h2', '修改密码'), element('p', '成功改密后所有原会话失效，需要重新登录。'));
    const current = field(form, '当前密码', 'current_password', 'password', 'current-password');
    const next = field(form, '新密码', 'new_password', 'password', 'new-password');
    const confirm = field(form, '确认新密码', 'confirm_password', 'password', 'new-password');
    secrets.push(current, next, confirm);
    label = '确认修改密码';
    submit = async () => {
      if (next.value !== confirm.value) { showError('两次新密码不一致。'); return; }
      const oldValue = current.value, newValue = next.value;
      for (const input of secrets) input.value = '';
      await store.changePassword(oldValue, newValue);
      navigate('login');
    };
  } else {
    const user = store.state.user;
    if (!user) return section;
    section.append(element('h2', '账户资料'));
    const facts = element('dl');
    for (const [name, value] of [['登录名', user.login], ['显示名称', user.display_name], ['角色', user.role === 'administrator' ? '管理员' : '成员']]) {
      facts.append(element('dt', name), element('dd', value));
    }
    section.append(facts);
    const display = field(form, '新的显示名称', 'display_name'); display.value = user.display_name;
    label = '保存显示名称';
    submit = () => store.updateName(display.value);
    section.append(button('重新读取资料', () => { void store.refresh(); }));
  }
  const send = element('button', label); send.type = 'submit';
  let submitting = false;
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    if (submitting || store.state.busy) return;
    submitting = true; send.disabled = true;
    void submit().catch((error: unknown) => showError(describeFailure(error))).finally(() => {
      for (const input of secrets) input.value = '';
      submitting = false; send.disabled = false;
    });
  });
  form.append(send); section.append(form);
  return section;
}
