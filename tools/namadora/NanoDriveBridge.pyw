"""Small Windows launcher for the MAME/NanoDrive bridge."""
import queue
import configparser
import os
import sys
import tempfile
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

try:
    import bridge
    from serial.tools import list_ports
except ImportError as exc:
    root = tk.Tk(); root.withdraw()
    messagebox.showerror('NanoDrive Bridge', f'起動に必要なモジュールがありません。\n{exc}\n\npyserial をインストールしてください。')
    root.destroy()
    raise SystemExit(1)

SETTINGS_PATH = Path(__file__).resolve().with_name('setting.ini')


def load_settings(path=SETTINGS_PATH):
    config = configparser.ConfigParser(interpolation=None)
    try:
        with path.open(encoding='utf-8') as source:
            config.read_file(source)
    except FileNotFoundError:
        return 'ND6', 'COM5', None
    except (OSError, UnicodeError, configparser.Error) as exc:
        return 'ND6', 'COM5', str(exc)
    device = config.get('bridge', 'device', fallback='ND6').upper()
    port = config.get('bridge', 'port', fallback='COM5').strip()
    return (device if device in ('ND6', 'ND8') else 'ND6'), port, None


def save_settings(device, port, path=SETTINGS_PATH):
    config = configparser.ConfigParser(interpolation=None)
    # Keep unrelated settings in an existing file.
    if path.exists():
        with path.open(encoding='utf-8') as source:
            config.read_file(source)
    if not config.has_section('bridge'):
        config.add_section('bridge')
    config.set('bridge', 'device', device)
    config.set('bridge', 'port', port)
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix='.setting-', suffix='.tmp', delete=False) as target:
            temp_name = target.name
            config.write(target)
        os.replace(temp_name, path)
    finally:
        if temp_name and os.path.exists(temp_name):
            os.unlink(temp_name)


class QueueWriter:
    def __init__(self, messages):
        self.messages = messages

    def write(self, text):
        if text:
            self.messages.put(('log', text))
        return len(text)

    def flush(self):
        pass


class App:
    def __init__(self, root):
        self.root = root
        root.title('NanoDrive MAME Bridge')
        root.geometry('650x390')
        root.minsize(500, 300)
        self.messages = queue.Queue()
        self.stop_event = None
        self.worker = None
        self.closing = False
        device, port, settings_error = load_settings()
        self.device = tk.StringVar(value=device)
        self.port = tk.StringVar(value=port)
        self.preferred_port = port
        self.available_ports = ()
        self.status = tk.StringVar(value='停止中')
        frame = ttk.Frame(root, padding=12); frame.pack(fill='both', expand=True)
        settings = ttk.Frame(frame); settings.pack(fill='x')
        ttk.Label(settings, text='機器').pack(side='left')
        self.device_choice = ttk.Combobox(settings, textvariable=self.device, values=('ND6', 'ND8'), state='readonly', width=7)
        self.device_choice.pack(side='left', padx=(5, 16))
        self.device_choice.bind('<<ComboboxSelected>>', self.selection_changed)
        ttk.Label(settings, text='COM ポート').pack(side='left')
        self.port_choice = ttk.Combobox(settings, textvariable=self.port, state='readonly', width=9,
                                       postcommand=self.refresh_ports)
        self.port_choice.pack(side='left', padx=5)
        self.port_choice.bind('<<ComboboxSelected>>', self.selection_changed)
        self.refresh_button = ttk.Button(settings, text='更新', command=self.refresh_ports, width=5)
        self.refresh_button.pack(side='left')
        self.start_button = ttk.Button(settings, text='開始', command=self.start); self.start_button.pack(side='left', padx=(12, 4))
        self.stop_button = ttk.Button(settings, text='停止', command=self.stop, state='disabled'); self.stop_button.pack(side='left')
        ttk.Label(frame, textvariable=self.status).pack(anchor='w', pady=(10, 5))
        log_frame = ttk.Frame(frame); log_frame.pack(fill='both', expand=True)
        self.log = tk.Text(log_frame, height=14, wrap='word', state='disabled')
        scrollbar = ttk.Scrollbar(log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side='right', fill='y'); self.log.pack(side='left', fill='both', expand=True)
        if settings_error:
            self.messages.put(('log', f'setting.ini の読み込みに失敗しました: {settings_error}\n'))
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.refresh_ports()
        root.after(100, self.poll)
        if self.port.get() in self.available_ports:
            root.after(100, self.start)

    def refresh_ports(self):
        try:
            self.available_ports = tuple(sorted({port.device for port in list_ports.comports()},
                                                key=lambda name: (len(name), name.upper())))
        except (OSError, ValueError) as exc:
            self.available_ports = ()
            self.messages.put(('log', f'COM ポートの一覧を取得できません: {exc}\n'))
        self.port_choice.configure(values=self.available_ports)
        if self.worker is None:
            if not self.available_ports:
                self.port.set('')
                self.port_choice.configure(state='disabled')
                self.start_button.configure(state='disabled')
                self.status.set('COM ポートが見つかりません。接続して「更新」を押してください')
            else:
                self.port_choice.configure(state='readonly')
                self.start_button.configure(state='normal')
                if self.port.get() not in self.available_ports:
                    selected = (self.preferred_port if self.preferred_port in self.available_ports
                                else self.available_ports[0])
                    self.port.set(selected)
                    if selected != self.preferred_port:
                        self.selection_changed()
                self.status.set('停止中')

    def selection_changed(self, _event=None):
        self.preferred_port = self.port.get()
        try:
            save_settings(self.device.get(), self.port.get())
        except (OSError, UnicodeError, configparser.Error) as exc:
            messagebox.showerror('NanoDrive Bridge', f'setting.ini を保存できません。\n{exc}')
        if self.port.get() in self.available_ports:
            self.status.set('停止中')

    def start(self):
        if self.worker is not None:
            return
        self.refresh_ports()
        port = self.port.get().strip()
        if port not in self.available_ports:
            messagebox.showerror('NanoDrive Bridge', '使用する COM ポートを一覧から選択してください。')
            return
        device = self.device.get().lower()
        self.stop_event = threading.Event()
        self.status.set('接続しています…')
        self.start_button.configure(state='disabled'); self.stop_button.configure(state='normal')
        self.port_choice.configure(state='disabled'); self.device_choice.configure(state='disabled')
        self.refresh_button.configure(state='disabled')

        def work():
            try:
                if device == 'nd6':
                    bridge.run_live(port, stop_event=self.stop_event)
                else:
                    bridge.run(port, device, stop_event=self.stop_event)
            except Exception as exc:
                self.messages.put(('error', str(exc)))
            finally:
                self.messages.put(('finished', ''))

        self.worker = threading.Thread(target=work, name='bridge', daemon=True)
        self.worker.start()

    def stop(self):
        if self.stop_event:
            self.stop_event.set()
            self.status.set('停止しています…')
            self.stop_button.configure(state='disabled')

    def close(self):
        self.closing = True
        if self.worker is None:
            self.root.destroy()
        else:
            self.stop()

    def poll(self):
        try:
            while True:
                kind, value = self.messages.get_nowait()
                if kind == 'log':
                    self.log.configure(state='normal'); self.log.insert('end', value)
                    self.log.see('end'); self.log.configure(state='disabled')
                    if 'Waiting for MAME plugin' in value: self.status.set('MAME の接続待ち')
                    elif 'MAME connected' in value: self.status.set('MAME 接続中')
                elif kind == 'error':
                    self.status.set('エラー')
                    self.log.configure(state='normal'); self.log.insert('end', f'エラー: {value}\n'); self.log.see('end'); self.log.configure(state='disabled')
                elif kind == 'finished':
                    self.worker = None
                    if self.closing:
                        self.root.destroy(); return
                    had_error = self.status.get() == 'エラー'
                    self.stop_button.configure(state='disabled')
                    self.device_choice.configure(state='readonly')
                    self.refresh_button.configure(state='normal')
                    self.refresh_ports()
                    if had_error: self.status.set('エラー')
        except queue.Empty:
            pass
        self.root.after(100, self.poll)


def main():
    root = tk.Tk()
    app = App(root)
    sys.stdout = QueueWriter(app.messages)
    sys.stderr = QueueWriter(app.messages)
    root.mainloop()


if __name__ == '__main__':
    main()
