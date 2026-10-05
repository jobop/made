export default {
  apiVersion: 1,
  kind: 'board-plugin',
  id: 'boot_caption',
  label: '按键回声',
  events: ['boot.click', 'boot.double', 'boot.triple', 'boot.long'],
  commands: ['caption.show'],
  probe: () => ({ available: true }),
  async onEvent({ event }) {
    const text = {
      'boot.click': '单击',
      'boot.double': '双击',
      'boot.triple': '三击',
      'boot.long': '长按',
    }[event.name];
    if (!text) return { commands: [] };
    return { commands: [{ name: 'caption.show', fields: { text } }] };
  },
};
