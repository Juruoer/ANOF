export function element<K extends keyof HTMLElementTagNameMap>(tag: K, content?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (content !== undefined) node.textContent = content;
  return node;
}
export function button(label: string, action: () => void): HTMLButtonElement {
  const node = element('button', label);
  node.type = 'button'; node.addEventListener('click', action); return node;
}
export function field(form: HTMLFormElement, label: string, name: string, type = 'text', autocomplete = 'off'): HTMLInputElement {
  const wrapper = element('label', label);
  const input = element('input');
  input.name = name; input.type = type; input.required = true;
  input.setAttribute('autocomplete', autocomplete);
  input.maxLength = type === 'password' ? 1024 : 128;
  wrapper.append(input); form.append(wrapper); return input;
}
