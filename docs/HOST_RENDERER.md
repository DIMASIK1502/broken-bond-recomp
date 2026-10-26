# Свой рендер: перехват D3D, Plume, XenosRecomp

Цель — рисовать BB без эмуляции Xenos: перехватывать вызовы D3D самой игры
(устройство, ресурсы, состояния, draw, resolve, present) и отдавать их в
Vulkan / D3D12 через [Plume](https://github.com/renderbag/plume). Шейдеры
переводятся заранее [XenosRecomp](https://github.com/hedge-dev/XenosRecomp).
Оба MIT. Этот же путь потом идёт на Android (Vulkan Plume, SPIR-V из кэша).

Архитектуру сверяем с README и `docs/` у
[Auremana666](https://github.com/Auremana666/Naruto-The-Broken-Bond-PC-Port) /
[jorjbarbosa](https://github.com/jorjbarbosa/broken-bond-recomp). Их исходники
GPL-3.0 — не открываем и не переносим (см. `DEBT_FURTHER.md`). Адреса BB
находим сами.

Дефолт остаётся Xenos, пока свой рендер не проходит критерий из
`DEBT_FURTHER.md`.

## Фазы

1. Зависимости и кэш шейдеров. **Готово**, ниже.
2. Карта D3D в BB: Create* / Set* / Draw* / Resolve / Swap, адреса и что
   каждая пишет в device. **Готово для бута и титула**:
   [`D3D_MAP.md`](D3D_MAP.md), [`D3D_STATES.md`](D3D_STATES.md).
3. Теневой режим: хуки пишут вызовы, рисует Xenos. Сверка draw и текстур.
4. Заставка на своём рендере через Plume Vulkan, свой swapchain, без xenos.
5. Меню, бой, сюжет: render target'ы, resolve, occlusion queries, катсцены.
6. 60+ FPS с интерполяцией между тиками (F4 в `DEBT_FURTHER.md`).
7. Android.

## Фаза 1

`setup.ps1` клонирует Plume `0ce184b` и XenosRecomp `990d03b` в `thirdparty/`
и накладывает `patches/xenosrecomp/broken-bond.patch`.

Цепочка сборки (цель `broken_bond_shader_cache`, кэш только в каталоге сборки):

1. `shader_db <sdb> extract <dir>` пишет каждый объект `xeshader.sdb` после
   zlib отдельным файлом. Это обычный контейнер D3D `0x102A1101` (vs) /
   `0x102A1100` (ps): все 8156 проходят проверку заголовка XenosRecomp,
   `virtualSize + physicalSize` равно размеру объекта.
2. XenosRecomp (собирается как отдельный инструмент, у него свои fmt и xxHash)
   переводит каталог в HLSL, DXC даёт SPIR-V и DXIL, всё ложится в
   `shader_cache.cpp`. Ключ записи — XXH3_64 по байтам контейнера.

Без патча XenosRecomp из 8156 шейдеров компилировались 6799. Причины:

- 1312 шейдеров читают текстуру из fetch-слота, которого нет в их таблице
  констант: 1041 пиксельный (`s1`–`s7`) и 271 вершинный (`s16`). Upstream
  кладёт индексы дескрипторов по имени. Патч кладёт их по слоту:
  `TEXTURE2D_INDEX(5)`, `SAMPLER_INDEX(5)`. Хост всё равно заполняет их из 32
  fetch-констант гостя.
- Те же 271 вершинный шейдер берут из слота 16 1D-текстуру. Upstream 1D не
  умеет; добавлены `Texture1D` (space5) и `tfetch1D`.
- 45 вершинных шейдеров со входами вне таблицы локаций Unleashed (`COLOR2/3`,
  `TEXCOORD2-4`). У BB ровно 16 входных семантик; таблица переписана под них.

Контракт шейдер ↔ хост — `src/gfx/xenos_shader_common.h`: его XenosRecomp
вставляет в каждый HLSL, а C++ берёт оттуда раскладку (`kDescriptorIndexBytes`,
смещения общих констант, `kVertexLocations`). Общие константы: 5 × 32 индекса
дескрипторов (640 байт), затем booleans, swapped texcoords, half pixel, alpha
threshold.

С патчем: 8156 из 8156 дают SPIR-V и DXIL, `spirv-val` (Vulkan 1.2, scalar
block layout) пропустил все 8156. Кэш: 5009 уникальных шейдеров, SPIR-V 16.8 МБ
(3.1 МБ zstd), DXIL 29.9 МБ (3.6 МБ zstd), перевод 26 с.

`render_probe` открывает Plume Vulkan и создаёт модуль из каждой записи кэша.
На Intel Arc (интегрированная, драйвер 0x19629C): 5009 из 5009 модулей за
28 мс, 4527 шейдеров со spec-константами.

Не проверено: пайплайны (нужны vertex declaration и render state), DXIL на
D3D12, совпадение картинки с Xenos.
