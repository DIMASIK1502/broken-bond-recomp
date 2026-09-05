#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Лаунчер Naruto: The Broken Bond — Qt (PySide6)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

os.chdir(Path(__file__).resolve().parent)
ROOT = Path(__file__).resolve().parent
EXE_NAME = "broken_bond.exe"
SETTINGS_FILE = ROOT / "launcher.json"
TOML_FILE = ROOT / "broken_bond.toml"
GAME_DIR = ROOT / "game"
LOG_DIR = ROOT / "logs"

FORCED = {
    "gpu_plugin": "xenos",
    "render_target_path_d3d12": "rov",
    "game_data_root": "game",
    "occlusion_query_enable": True,
}

ANISO = [("Как в игре", -1), ("Выкл.", 0), ("2x", 2), ("4x", 3), ("8x", 4), ("16x", 5)]
PRESENT = [
    ("Билинейный", "bilinear"),
    ("CAS (резкость)", "cas"),
    ("FSR 1", "fsr"),
    ("FSR 2", "fsr2"),
    ("FSR 3", "fsr3"),
]
FXAA = [("Нет", "none"), ("FXAA", "fxaa"), ("FXAA Extreme", "fxaa_extreme")]
SCALE = [("1x (натив 1280×512)", 1), ("2x (~2K, 2560×1024)", 2), ("3x (тяжело)", 3)]

KEYBINDS = [
    ("keybind_a", "A (подтверждение / атака)"),
    ("keybind_b", "B (отмена)"),
    ("keybind_x", "X"),
    ("keybind_y", "Y"),
    ("keybind_start", "Start (пауза / меню)"),
    ("keybind_back", "Back"),
    ("keybind_left_trigger", "LT"),
    ("keybind_right_trigger", "RT"),
    ("keybind_left_shoulder", "LB"),
    ("keybind_right_shoulder", "RB"),
    ("keybind_lstick_up", "Левый стик вверх"),
    ("keybind_lstick_down", "Левый стик вниз"),
    ("keybind_lstick_left", "Левый стик влево"),
    ("keybind_lstick_right", "Левый стик вправо"),
    ("keybind_lstick_press", "Л3"),
    ("keybind_rstick_up", "Правый стик вверх"),
    ("keybind_rstick_down", "Правый стик вниз"),
    ("keybind_rstick_left", "Правый стик влево"),
    ("keybind_rstick_right", "Правый стик вправо"),
    ("keybind_rstick_press", "R3"),
    ("keybind_dpad_up", "D-pad вверх"),
    ("keybind_dpad_down", "D-pad вниз"),
    ("keybind_dpad_left", "D-pad влево"),
    ("keybind_dpad_right", "D-pad вправо"),
]

DEFAULT_BINDS = {
    "keybind_a": "Semicolon,Space",
    "keybind_b": "Quote,Backspace",
    "keybind_x": "L",
    "keybind_y": "P",
    "keybind_start": "X,Return",
    "keybind_back": "Z,Tab",
    "keybind_left_trigger": "Q,I",
    "keybind_right_trigger": "E,O",
    "keybind_left_shoulder": "1",
    "keybind_right_shoulder": "3",
    "keybind_lstick_up": "W",
    "keybind_lstick_down": "S",
    "keybind_lstick_left": "A",
    "keybind_lstick_right": "D",
    "keybind_lstick_press": "F",
    "keybind_rstick_up": "Up",
    "keybind_rstick_down": "Down",
    "keybind_rstick_left": "Left",
    "keybind_rstick_right": "Right",
    "keybind_rstick_press": "K",
    "keybind_dpad_up": "Shift+Up",
    "keybind_dpad_down": "Shift+Down",
    "keybind_dpad_left": "Shift+Left",
    "keybind_dpad_right": "Shift+Right",
}

PRESETS: dict[str, dict[str, Any]] = {
    "performance": {
        "resolution_scale": 1,
        "present_effect": "bilinear",
        "swap_post_effect": "none",
        "anisotropic_override": 3,
        "present_dither": False,
        "vsync": True,
        "fullscreen": True,
        "async_shader_compilation": True,
        "d3d12_pipeline_creation_threads": -1,
        "texture_cache_memory_limit_soft": 384,
        "texture_cache_memory_limit_hard": 768,
    },
    "medium": {
        "resolution_scale": 1,
        "present_effect": "cas",
        "swap_post_effect": "none",
        "anisotropic_override": 5,
        "present_dither": False,
        "vsync": True,
        "fullscreen": True,
        "async_shader_compilation": True,
        "d3d12_pipeline_creation_threads": 8,
        "texture_cache_memory_limit_soft": 1024,
        "texture_cache_memory_limit_hard": 2048,
    },
    "quality": {
        "resolution_scale": 2,
        "present_effect": "cas",
        "swap_post_effect": "none",
        "anisotropic_override": 5,
        "present_dither": True,
        "vsync": True,
        "fullscreen": True,
        "async_shader_compilation": True,
        "d3d12_pipeline_creation_threads": 8,
        "texture_cache_memory_limit_soft": 1024,
        "texture_cache_memory_limit_hard": 2048,
    },
}

PRESET_LABELS = [
    ("performance", "Производительность", "Натив 1280×512, меньше фризов"),
    ("medium", "Среднее", "Натив + CAS, резче без 2x"),
    ("quality", "Качество", "Внутренний 2K. Первый бой компилирует шейдеры"),
    ("custom", "Свои настройки", "Всё ниже можно крутить вручную"),
]


def toml_value(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return str(v)
    s = str(v).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{s}"'


def write_toml(settings: dict[str, Any]) -> None:
    lines = [
        "# Сгенерировано лаунчером. F4 в игре перезапишет этот файл.",
        f'gpu_plugin = {toml_value(FORCED["gpu_plugin"])}',
        f'render_target_path_d3d12 = {toml_value(FORCED["render_target_path_d3d12"])}',
        f'game_data_root = {toml_value(FORCED["game_data_root"])}',
        f'occlusion_query_enable = {toml_value(FORCED["occlusion_query_enable"])}',
        "",
        f'mnk_mode = {toml_value(settings["mnk_mode"])}',
        f'mnk_mouse = {toml_value(settings["mnk_mouse"])}',
        f'mnk_sensitivity = {toml_value(settings["mnk_sensitivity"])}',
        "",
        f'resolution_scale = {toml_value(settings["resolution_scale"])}',
        f'present_effect = {toml_value(settings["present_effect"])}',
        f'swap_post_effect = {toml_value(settings["swap_post_effect"])}',
        f'anisotropic_override = {toml_value(settings["anisotropic_override"])}',
        f'present_dither = {toml_value(settings["present_dither"])}',
        f'vsync = {toml_value(settings["vsync"])}',
        f'fullscreen = {toml_value(settings["fullscreen"])}',
        f'async_shader_compilation = {toml_value(settings["async_shader_compilation"])}',
        f'd3d12_pipeline_creation_threads = {toml_value(settings["d3d12_pipeline_creation_threads"])}',
        f'texture_cache_memory_limit_soft = {toml_value(settings["texture_cache_memory_limit_soft"])}',
        f'texture_cache_memory_limit_hard = {toml_value(settings["texture_cache_memory_limit_hard"])}',
        "",
    ]
    for key, _ in KEYBINDS:
        lines.append(f"{key} = {toml_value(settings[key])}")
    TOML_FILE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def default_settings() -> dict[str, Any]:
    s: dict[str, Any] = {
        "preset": "quality",
        "mnk_mode": True,
        "mnk_mouse": False,
        "mnk_sensitivity": 1.0,
    }
    s.update(PRESETS["quality"])
    s.update(DEFAULT_BINDS)
    return s


def load_settings() -> dict[str, Any]:
    s = default_settings()
    if SETTINGS_FILE.is_file():
        try:
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            if isinstance(data, dict):
                s.update(data)
        except (OSError, json.JSONDecodeError):
            pass
    return s


def save_settings_file(settings: dict[str, Any]) -> None:
    SETTINGS_FILE.write_text(json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8")


def apply_dark_palette(app: Any) -> None:
    from PySide6.QtGui import QColor, QPalette
    from PySide6.QtWidgets import QStyleFactory

    app.setStyle(QStyleFactory.create("Fusion"))
    p = QPalette()
    p.setColor(QPalette.ColorRole.Window, QColor(28, 30, 34))
    p.setColor(QPalette.ColorRole.WindowText, QColor(230, 230, 230))
    p.setColor(QPalette.ColorRole.Base, QColor(22, 24, 28))
    p.setColor(QPalette.ColorRole.AlternateBase, QColor(36, 38, 44))
    p.setColor(QPalette.ColorRole.ToolTipBase, QColor(22, 24, 28))
    p.setColor(QPalette.ColorRole.ToolTipText, QColor(230, 230, 230))
    p.setColor(QPalette.ColorRole.Text, QColor(230, 230, 230))
    p.setColor(QPalette.ColorRole.Button, QColor(42, 45, 52))
    p.setColor(QPalette.ColorRole.ButtonText, QColor(230, 230, 230))
    p.setColor(QPalette.ColorRole.Highlight, QColor(196, 92, 26))
    p.setColor(QPalette.ColorRole.HighlightedText, QColor(255, 255, 255))
    p.setColor(QPalette.ColorRole.PlaceholderText, QColor(140, 140, 140))
    app.setPalette(p)


class Combo:
    def __init__(self, pairs: list[tuple[str, Any]]) -> None:
        from PySide6.QtWidgets import QComboBox

        self.pairs = pairs
        self.box = QComboBox()
        for label, _value in pairs:
            self.box.addItem(label)

    def set_value(self, value: Any) -> None:
        for i, (_label, v) in enumerate(self.pairs):
            if v == value:
                self.box.setCurrentIndex(i)
                return

    def value(self) -> Any:
        return self.pairs[self.box.currentIndex()][1]


class LauncherWindow:
    def __init__(self) -> None:
        from PySide6.QtWidgets import (
            QGroupBox,
            QHBoxLayout,
            QLabel,
            QMainWindow,
            QMessageBox,
            QPushButton,
            QRadioButton,
            QTabWidget,
            QVBoxLayout,
            QWidget,
        )

        self._QMessageBox = QMessageBox
        self.saved = load_settings()
        self._applying = True

        self.win = QMainWindow()
        self.win.setWindowTitle("Naruto: The Broken Bond")
        self.win.resize(860, 640)
        self.win.setMinimumSize(760, 520)

        central = QWidget()
        self.win.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(16, 16, 16, 16)
        root.setSpacing(12)

        title = QLabel("Naruto: The Broken Bond")
        title.setStyleSheet("font-size: 22px; font-weight: 700;")
        sub = QLabel("ReXGlue · лаунчер")
        sub.setStyleSheet("color: #9aa0a6; font-size: 13px;")
        root.addWidget(title)
        root.addWidget(sub)

        preset_box = QGroupBox("Пресет графики")
        preset_l = QVBoxLayout(preset_box)
        self.preset_buttons: dict[str, QRadioButton] = {}
        for key, name, hint in PRESET_LABELS:
            rb = QRadioButton(f"{name}  —  {hint}")
            rb.toggled.connect(lambda checked, k=key: self._on_preset(k) if checked else None)
            self.preset_buttons[key] = rb
            preset_l.addWidget(rb)
        root.addWidget(preset_box)

        tabs = QTabWidget()
        tabs.addTab(self._gfx_tab(), "Графика")
        tabs.addTab(self._ctl_tab(), "Управление")
        tabs.addTab(self._about_tab(), "Справка")
        root.addWidget(tabs, 1)

        buttons = QHBoxLayout()
        save_btn = QPushButton("Сохранить")
        save_btn.clicked.connect(self.save_only)
        play_btn = QPushButton("Сохранить и запустить")
        play_btn.setDefault(True)
        play_btn.setStyleSheet(
            "QPushButton { background: #c45c1a; color: white; font-weight: 700; "
            "padding: 8px 18px; border: none; border-radius: 4px; }"
            "QPushButton:hover { background: #d46a28; }"
        )
        play_btn.clicked.connect(self.save_and_launch)
        buttons.addWidget(save_btn)
        buttons.addStretch(1)
        buttons.addWidget(play_btn)
        root.addLayout(buttons)

        self._load_into_ui(self.saved)
        key = str(self.saved.get("preset", "quality"))
        if key not in self.preset_buttons:
            key = "custom"
        self.preset_buttons[key].setChecked(True)
        self._applying = False

    def _combo_row(self, form: Any, label: str, pairs: list[tuple[str, Any]]) -> Combo:
        from PySide6.QtWidgets import QLabel

        combo = Combo(pairs)
        combo.box.currentIndexChanged.connect(self._mark_custom)
        form.addRow(QLabel(label), combo.box)
        return combo

    def _gfx_tab(self) -> Any:
        from PySide6.QtWidgets import (
            QCheckBox,
            QFormLayout,
            QLabel,
            QSpinBox,
            QVBoxLayout,
            QWidget,
        )

        page = QWidget()
        layout = QVBoxLayout(page)
        form = QFormLayout()
        form.setSpacing(8)
        self.scale = self._combo_row(form, "Внутреннее разрешение", SCALE)
        self.present = self._combo_row(form, "Фильтр вывода на экран", PRESENT)
        self.fxaa = self._combo_row(form, "Сглаживание кадра (FXAA)", FXAA)
        self.aniso = self._combo_row(form, "Анизотропная фильтрация", ANISO)

        self.threads = QSpinBox()
        self.threads.setRange(-1, 16)
        self.threads.valueChanged.connect(self._mark_custom)
        form.addRow("Потоки компиляции PSO (−1 = авто)", self.threads)
        layout.addLayout(form)

        self.fullscreen = QCheckBox("Полный экран")
        self.vsync = QCheckBox("Вертикальная синхронизация (VSync)")
        self.dither = QCheckBox("Дизеринг (меньше полос на градиентах)")
        self.async_sh = QCheckBox("Асинхронная компиляция шейдеров")
        for cb in (self.fullscreen, self.vsync, self.dither, self.async_sh):
            cb.toggled.connect(self._mark_custom)
            layout.addWidget(cb)

        note = QLabel(
            "ROV для NVIDIA включён всегда (иначе чёрный экран в бою). "
            "2x красивее, но первый раз каждого приёма может подфризить. "
            "FXAA мылит поверх нативного 4xMSAA — лучше CAS или «Нет»."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color: #9aa0a6;")
        layout.addWidget(note)
        layout.addStretch(1)
        return page

    def _ctl_tab(self) -> Any:
        from PySide6.QtWidgets import (
            QCheckBox,
            QDoubleSpinBox,
            QFormLayout,
            QHBoxLayout,
            QLabel,
            QLineEdit,
            QPushButton,
            QScrollArea,
            QVBoxLayout,
            QWidget,
        )

        page = QWidget()
        layout = QVBoxLayout(page)
        self.mnk_mode = QCheckBox("Клавиатура и мышь (эмуляция геймпада)")
        self.mnk_mouse = QCheckBox("Мышь = правый стик (камера)")
        layout.addWidget(self.mnk_mode)
        layout.addWidget(self.mnk_mouse)

        sens_row = QHBoxLayout()
        sens_row.addWidget(QLabel("Чувствительность мыши"))
        self.mnk_sens = QDoubleSpinBox()
        self.mnk_sens.setRange(0.2, 3.0)
        self.mnk_sens.setSingleStep(0.1)
        self.mnk_sens.setDecimals(2)
        sens_row.addWidget(self.mnk_sens)
        sens_row.addStretch(1)
        layout.addLayout(sens_row)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        inner = QWidget()
        form = QFormLayout(inner)
        self.bind_edits: dict[str, QLineEdit] = {}
        for key, label in KEYBINDS:
            edit = QLineEdit()
            self.bind_edits[key] = edit
            form.addRow(label, edit)
        scroll.setWidget(inner)
        layout.addWidget(scroll, 1)

        reset = QPushButton("Сбросить клавиши")
        reset.clicked.connect(self._reset_binds)
        layout.addWidget(reset)
        return page

    def _about_tab(self) -> Any:
        from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

        page = QWidget()
        layout = QVBoxLayout(page)
        text = QLabel(
            "Порт на PC через ReXGlue.\n\n"
            "• Сохранения: Документы\\broken_bond\\\n"
            "• Не нажимай F4 «Save to config» в игре — собьёт конфиг лаунчера.\n"
            "• Первый запуск и новые приёмы компилируют шейдеры (короткий фриз).\n"
            "• A = Space / точка с запятой, Start = Enter / X.\n"
            "• Японская и английская озвучка должны проходить катсцены."
        )
        text.setWordWrap(True)
        layout.addWidget(text)
        layout.addStretch(1)
        return page

    def _mark_custom(self, *_args: object) -> None:
        if self._applying:
            return
        self._applying = True
        self.preset_buttons["custom"].setChecked(True)
        self._applying = False

    def _on_preset(self, key: str) -> None:
        if self._applying or key == "custom" or key not in PRESETS:
            return
        self._applying = True
        merged = dict(self.collect())
        merged.update(PRESETS[key])
        merged["preset"] = key
        self._load_into_ui(merged)
        self._applying = False

    def _load_into_ui(self, s: dict[str, Any]) -> None:
        self.scale.set_value(int(s.get("resolution_scale", 2)))
        self.present.set_value(str(s.get("present_effect", "cas")))
        self.fxaa.set_value(str(s.get("swap_post_effect", "none")))
        self.aniso.set_value(int(s.get("anisotropic_override", 5)))
        self.threads.setValue(int(s.get("d3d12_pipeline_creation_threads", 8)))
        self.fullscreen.setChecked(bool(s.get("fullscreen", True)))
        self.vsync.setChecked(bool(s.get("vsync", True)))
        self.dither.setChecked(bool(s.get("present_dither", False)))
        self.async_sh.setChecked(bool(s.get("async_shader_compilation", True)))
        self.mnk_mode.setChecked(bool(s.get("mnk_mode", True)))
        self.mnk_mouse.setChecked(bool(s.get("mnk_mouse", False)))
        self.mnk_sens.setValue(float(s.get("mnk_sensitivity", 1.0)))
        for key, _label in KEYBINDS:
            self.bind_edits[key].setText(str(s.get(key, DEFAULT_BINDS[key])))

    def _reset_binds(self) -> None:
        for key, val in DEFAULT_BINDS.items():
            self.bind_edits[key].setText(val)
        self._mark_custom()

    def collect(self) -> dict[str, Any]:
        preset = "custom"
        for key, btn in self.preset_buttons.items():
            if btn.isChecked():
                preset = key
                break
        s: dict[str, Any] = {
            "preset": preset,
            "mnk_mode": self.mnk_mode.isChecked(),
            "mnk_mouse": self.mnk_mouse.isChecked(),
            "mnk_sensitivity": round(float(self.mnk_sens.value()), 2),
            "resolution_scale": int(self.scale.value()),
            "present_effect": str(self.present.value()),
            "swap_post_effect": str(self.fxaa.value()),
            "anisotropic_override": int(self.aniso.value()),
            "present_dither": self.dither.isChecked(),
            "vsync": self.vsync.isChecked(),
            "fullscreen": self.fullscreen.isChecked(),
            "async_shader_compilation": self.async_sh.isChecked(),
            "d3d12_pipeline_creation_threads": int(self.threads.value()),
            "texture_cache_memory_limit_soft": int(
                PRESETS.get(preset, PRESETS["quality"]).get(
                    "texture_cache_memory_limit_soft", 1024
                )
            ),
            "texture_cache_memory_limit_hard": int(
                PRESETS.get(preset, PRESETS["quality"]).get(
                    "texture_cache_memory_limit_hard", 2048
                )
            ),
        }
        if preset == "custom":
            s["texture_cache_memory_limit_soft"] = 1024
            s["texture_cache_memory_limit_hard"] = 2048
        for key, edit in self.bind_edits.items():
            s[key] = edit.text().strip()
        return s

    def save_only(self) -> None:
        try:
            s = self.collect()
            save_settings_file(s)
            write_toml(s)
        except OSError as exc:
            self._QMessageBox.critical(self.win, "Ошибка", f"Не удалось сохранить конфиг:\n{exc}")
            return
        self._QMessageBox.information(self.win, "Лаунчер", "Настройки сохранены в broken_bond.toml")

    def save_and_launch(self) -> None:
        try:
            s = self.collect()
            save_settings_file(s)
            write_toml(s)
        except OSError as exc:
            self._QMessageBox.critical(self.win, "Ошибка", f"Не удалось сохранить конфиг:\n{exc}")
            return
        self.launch()

    def launch(self) -> None:
        exe = ROOT / EXE_NAME
        if not exe.is_file():
            self._QMessageBox.critical(self.win, "Ошибка", f"Нет {EXE_NAME} рядом с лаунчером.")
            return
        if not (GAME_DIR / "default.xex").is_file():
            self._QMessageBox.critical(self.win, "Ошибка", "Нет game\\default.xex.")
            return
        LOG_DIR.mkdir(exist_ok=True)
        s = self.collect()
        cmd = [
            str(exe),
            f"--game_data_root={GAME_DIR}",
            "--gpu_plugin=xenos",
            "--render_target_path_d3d12=rov",
            f"--resolution_scale={s['resolution_scale']}",
            f"--present_effect={s['present_effect']}",
            f"--swap_post_effect={s['swap_post_effect']}",
            f"--log_file={LOG_DIR / 'run.log'}",
            "--log_level=info",
        ]
        if s["mnk_mode"]:
            cmd.append("--mnk_mode")
        try:
            subprocess.Popen(cmd, cwd=str(ROOT))
        except OSError as exc:
            self._QMessageBox.critical(self.win, "Ошибка", f"Не удалось запустить игру:\n{exc}")
            return
        self.win.close()

    def show(self) -> None:
        self.win.show()


def main() -> int:
    from PySide6.QtWidgets import QApplication

    app = QApplication(sys.argv)
    app.setApplicationName("Broken Bond Launcher")
    apply_dark_palette(app)
    window = LauncherWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
