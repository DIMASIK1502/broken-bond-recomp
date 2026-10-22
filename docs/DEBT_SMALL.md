# Мелкий долг

Флаги и лаунчер. Без гостевых адресов. Не копировать Rise.

## Уже есть в нашем стеке

- `execute_unclipped_draw_vs_on_cpu` в текущем SDK по умолчанию **true**
  (то, что Rise когда-то патчил).
- `NvOptimusEnablement` и `AmdPowerXpressRequestHighPerformance` экспортирует
  `rexglue-sdk` (`graphics_system.cpp`). Отдельный экспорт в title не нужен.
- ROV (`render_target_path_d3d12=rov`) — наш фикс чёрного боя, не их.
- XMA drained EOS — наш фикс JP-озвучки.
- Захват клавиш в лаунчере по нажатию.
- Пресеты разрешения 1×–5× с подсказкой под монитор.

## Сделать / проверить

- [x] `d3d12_readback_resolve` — галка в лаунчере, в toml/CLI. Дефолт выкл.
- [x] `d3d12_adapter` — список DXGI-адаптеров (`-1` = авто).
- [x] `user_language` — EN/JP/DE/FR/ES/IT в лаунчере.
- [x] `skip_boot_logos` — галка пишет toml/CLI. Хук `.bik` — среднее.
- [x] F3 / F4 / ` в справке лаунчера.
- [x] Счётчик гостевого `XE_SWAP` есть (`GetPresentedFrameSerial` на
      `release/v0.10.0`). Это лог раз в 5 с, не оверлей. Их
      `guest-frame-stats.patch` не брать.
- [ ] `occlusion_query_enable` не выключать (уже так).
- [ ] Не класть ROV в `OnPostSetup` (kInitOnly).
- [ ] Патчи SDK, если появятся, только в `patches/sdk/` + скрипт apply.
      `.references/` в git не коммитить.

## Не делать «мелочью»

- Нативный рендерер / 60 FPS — `DEBT_FURTHER.md`.
- Ультраширок, мышь-камера, skip BIK хук — `DEBT_MEDIUM.md`.
