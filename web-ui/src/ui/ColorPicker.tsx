import { useState } from 'react';
import { Dialog } from '@base-ui/react/dialog';

const colors = ['#00ff00', '#ffff00', '#0064ff', '#280050', '#ff0000', '#0050ff', '#a000ff', '#ffffff', '#00ff80', '#ff8000', '#ff00ff', '#000000'];

export function ColorPicker({ value, label, onChange }: { value: string; label: string; onChange: (value: string) => void }) {
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState(value);
  const valid = /^#[\da-f]{6}$/i.test(draft);
  return <Dialog.Root open={open} onOpenChange={(next) => { if (next) setDraft(value); setOpen(next); }}>
    <Dialog.Trigger className="color-picker-trigger" aria-label={`${label}，当前 ${value}`} style={{ backgroundColor: value }} />
    <Dialog.Portal>
      <Dialog.Backdrop className="newui-dialog-backdrop" />
      <Dialog.Popup className="newui-dialog-popup color-picker-popup">
        <Dialog.Title>{label}</Dialog.Title>
        <Dialog.Description>选择预设色或输入 HEX 色值。完成后，点击页面的“保存灯光设置”生效。</Dialog.Description>
        <div className="color-picker-swatches" aria-label="预设颜色">
          {colors.map((color) => <button type="button" key={color} aria-label={color} aria-pressed={draft.toLowerCase() === color} style={{ backgroundColor: color }} onClick={() => setDraft(color)}><span>{draft.toLowerCase() === color ? '✓' : ''}</span></button>)}
        </div>
        <label className="color-picker-hex">HEX 色值<input value={draft} maxLength={7} spellCheck={false} autoComplete="off" aria-invalid={!valid} onChange={(event) => setDraft(event.target.value)} /></label>
        <p className="surface-help" role="status">{valid ? `已选颜色 ${draft.toUpperCase()}` : '请输入 # 和六位十六进制数字，例如 #0064FF。'}</p>
        <div className="newui-dialog-actions">
          <Dialog.Close className="secondary-button">取消</Dialog.Close>
          <button type="button" className="primary-button" disabled={!valid} onClick={() => { if (valid) { onChange(draft.toLowerCase()); setOpen(false); } }}>使用此颜色</button>
        </div>
      </Dialog.Popup>
    </Dialog.Portal>
  </Dialog.Root>;
}
