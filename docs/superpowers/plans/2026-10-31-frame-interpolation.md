# Интерполяция кадров: план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** при `--native_interpolate=true` показывать 60 FPS там, где игра шагает
на 30 Гц, проигрывая журнал кадра с константами и костями посередине между
двумя последними шагами.

**Architecture:** чистая логика (смешивание, сопоставление, пороги) живёт в
`src/render/interpolation.{h,cpp}` без Plume и проверяется отдельным тестом.
`Renderer` разбирает гостевые вызовы в записи журнала (`DrawOp`, `ClearOp`,
`ResolveOp`) и исполняет их одной функцией, так что кадр можно исполнить
второй раз. `Present` при включённом флаге проигрывает журнал со смешанными
данными, показывает промежуточный кадр, затем сохранённый настоящий.

**Tech Stack:** C++20, Plume (Vulkan), ReXGlue cvars, CMake/Ninja (clang).

**Spec:** `docs/superpowers/specs/2026-10-31-frame-interpolation-design.md`

## Global Constraints

- Всё новое поведение только при `--native_interpolate=true`; по умолчанию
  `false`. Без флага рендеринг, swapchain (2 изображения) и производительность
  как в коммите d221beb.
- Интерполяция только когда последний запрошенный игрой интервал флипа равен 2.
  При интервале 1 (бой) кадры идут как без флага.
- Смешивание ровно 0,5, без экстраполяции.
- Стартовый порог доли сопоставленных отрисовок 90 %.
- Без пары отрисовка рисуется с данными кадра N.
- Код GPL-портов не смотрим и не копируем.
- Сборка: PATH с `C:\Program Files\LLVM\bin` и CMake/Ninja из VS BuildTools,
  `cmake --build out/build/win-amd64-vulkan --target <target>`.

## Review Focus

1. Смена режима 30↔60 (вход в бой и выход): первый кадр после смены без
   промежуточного, без мигания старой картинкой.
2. Изменение размера окна или пересоздание целей между кадрами: журнал прошлого
   кадра ссылается на удалённые объекты; промежуточный кадр пропускается, без
   вылета.
3. Кадр без Present-цели (загрузка, Bink): журнал пустой, ничего не
   проигрывается.
4. Переполнение кольца загрузки при проигрывании: промежуточный кадр
   пропускается с причиной «upload ring full», настоящий показывается.
5. Окно свёрнуто или swapchain не выдал изображение: показ как без флага.

Тесты: пункт 1 — Task 6 (`ShouldInterpolate`), пункты 2–5 — Task 4 (шаги с
проверкой счётчиков пропуска в логе).

---

### Task 1: Чистая логика интерполяции и её тест

**Files:**
- Create: `src/render/interpolation.h`, `src/render/interpolation.cpp`
- Create: `tools/interpolation_test.cpp`
- Modify: `CMakeLists.txt` (исходник в `broken_bond`, новая цель `interpolation_test`)

**Interfaces:**
- Produces (namespace `broken_bond::render::interp`):
  - `uint64_t DrawKey(uint64_t vs_key, uint64_t ps_key, const uint32_t* stream_addresses, uint32_t stream_count, uint32_t index_address, uint32_t index_start, uint32_t count, const uint32_t* texture_indices, uint32_t texture_count)`
  - `std::vector<int32_t> MatchDraws(const std::vector<uint64_t>& prev, const std::vector<uint64_t>& cur)` — для каждой отрисовки `cur` индекс пары в `prev` или -1; k-я отрисовка с ключом K в `cur` берёт k-ю с ключом K в `prev`.
  - `float MatchedShare(const std::vector<int32_t>& match)` — доля `>= 0`, для пустого вектора 0.
  - `void LerpFloats(const float* prev, const float* cur, float* out, size_t count, float t)` — неконечное значение (NaN/Inf) в любом входе даёт `cur[i]`.
  - `void LerpHalfs(const uint16_t* prev, const uint16_t* cur, uint16_t* out, size_t count, float t)` — IEEE half, то же правило для неконечных.
  - `bool ShouldInterpolate(bool enabled, uint32_t flip_interval, bool have_prev, uint32_t prev_interval)` — true только при `enabled && flip_interval == 2 && have_prev && prev_interval == 2`.
  - `bool Jumped(const float* prev, const float* cur, size_t count, float threshold)` — true, если хоть у одного i `|cur-prev| > threshold * max(1, |prev|)`; неконечные пары пропускаются. Порог смены плана в Task 6: 0,5 по первым 16 float (c0–c3) констант VS.

- [ ] **Step 1: Write the failing test** `tools/interpolation_test.cpp` — `main()` с `CHECK(cond)` (печать строки и `return 1`):
  - `MatchDraws({A,B,A},{A,A,C,B}) == {0,2,-1,1}`
  - `MatchedShare({0,2,-1,1}) == 0.75f`, `MatchedShare({}) == 0.0f`
  - `LerpFloats({0,10},{2,20},out,2,0.5f)` → `{1,15}`; `prev[0]=NaN` → `out[0]==cur[0]`
  - `LerpHalfs` для 0x3C00 (1.0) и 0x4000 (2.0) при t=0.5 → 0x3E00 (1.5); `prev=0x7C00` (Inf) → `cur`
  - `DrawKey` меняется при смене любого аргумента (по одному тесту на vs_key, адрес потока, index_start, индекс текстуры)
  - `Jumped({1,2},{1.2f,2},2,0.5f)==false`, `Jumped({1,2},{3,2},2,0.5f)==true`, `Jumped({0},{0.4f},1,0.5f)==false`, `Jumped({NaN},{5},1,0.5f)==false`
  - `ShouldInterpolate(true,2,true,2)==true`; `(false,2,true,2)`, `(true,1,true,2)`, `(true,2,false,2)`, `(true,2,true,1)` → false
- [ ] **Step 2: Run** `cmake --build out/build/win-amd64-vulkan --target interpolation_test` — ожидается ошибка компиляции (нет `interpolation.h`).
- [ ] **Step 3: Implement** функции из Interfaces. Половинные float: перевод half↔float своими функциями (round-to-nearest-even при обратном переводе).
- [ ] **Step 4: Run** `out/build/win-amd64-vulkan/interpolation_test.exe` — вывод `interpolation_test: ok`, код 0.
- [ ] **Step 5: Commit** `feat: interpolation helpers with a unit test`

### Task 2: Флаг и интервал флипа в рендерере

**Files:**
- Modify: `src/render/renderer.h`, `src/render/renderer.cpp`, `src/host/guest_present.cpp` (хук `sub_821B16B0`)

**Interfaces:**
- Produces: `void Renderer::SetFlipInterval(uint32_t interval)` (потокобезопасно, атомик); cvar `native_interpolate` (bool, `false`, kInitOnly, группа "Game", описание: «With renderer=native, draw an in-between frame when the game steps at 30 Hz (shows 60 FPS one step late)»).
- Хук зовёт `g_native->SetFlipInterval(interval)` с интервалом, который запросила игра (до подмены `present_interval`).
- При флаге swapchain создаётся с 3 изображениями, без флага с 2.

- [ ] **Step 1:** добавить cvar, `SetFlipInterval`, вызов из хука, выбор числа изображений swapchain.
- [ ] **Step 2: Verify** заставка без флага: в логе `guest present 5s` 30 Гц, `native 5s` без изменений; с флагом: запускается, валидация Vulkan чистая (`run_shot.ps1` с переменными слоя валидации, лог пустой).
- [ ] **Step 3: Commit** `feat: native_interpolate option and flip interval in the renderer`

### Task 3: Журнал кадра (разбор и исполнение раздельно, поведение прежнее)

**Files:**
- Modify: `src/render/renderer.cpp`

**Interfaces:**
- Produces (внутри `State`):
  - `struct DrawOp { Target* target; plume::RenderPipeline* pipeline; uint64_t vs_constants, ps_constants, shared; plume::RenderViewport viewport; std::array<plume::RenderVertexBufferView, N> views; std::array<uint32_t, N> strides; uint32_t slot_count; bool indexed; uint64_t index_ring; uint32_t index_count, vertex_count; uint64_t key; uint32_t texture1d_slots[DESCRIPTOR_SLOT_COUNT]; bool uploaded; }` (N — нынешний размер массива слотов в `Draw`).
  - `struct ClearOp { Target* target; uint32_t flags; plume::RenderRect rect; plume::RenderColor color; float z; uint32_t stencil; }`
  - `struct ResolveOp` — всё, что `Resolve` вычисляет из гостевой памяти: источник (цвет/глубина), `Target*`, src-прямоугольник, точка назначения, ключ и указатель на `Texture` назначения, флаги очистки, цвет очистки, z.
  - `struct FrameOp { enum Kind {kDraw, kClear, kResolve} kind; uint32_t index; }` и `std::vector<FrameOp> journal; std::vector<DrawOp> draw_ops; std::vector<ClearOp> clear_ops; std::vector<ResolveOp> resolve_ops;` — очищаются в `BeginFrame`.
  - `void ExecuteDraw(const DrawOp&)`, `void ExecuteClear(const ClearOp&)`, `void ExecuteResolve(const ResolveOp&)` — то, что сейчас пишут в command list `Draw`, `Clear` и `Resolve` после разбора.
- `Draw`/`Clear`/`Resolve` разбирают гостевые данные в Op, кладут его в журнал (только при флаге) и вызывают `Execute*`. Ключ `DrawOp::key` — `interp::DrawKey(...)` из Task 1 с гостевыми адресами потоков, индексного буфера и индексами 2D-текстур.

- [ ] **Step 1:** вынести исполнение в `Execute*`, без журнала. Собрать.
- [ ] **Step 2: Verify** без флага: заставка рисуется как раньше (скриншот `run_shot.ps1` сравнить с `logs`-скриншотом до изменения), `native 5s` с теми же `drawn`, валидация чистая.
- [ ] **Step 3:** запись журнала при флаге; в `native 5s` добавить `journal=<ops за последний кадр>`.
- [ ] **Step 4: Verify** с флагом: `journal=` больше 0 на заставке, валидация чистая.
- [ ] **Step 5: Commit** `refactor: split native draws, clears and resolves into recorded ops`

### Task 4: Проигрывание журнала и двойной показ (без смешивания)

**Files:**
- Modify: `src/render/renderer.cpp`

**Interfaces:**
- Consumes: журнал из Task 3, `SetFlipInterval` из Task 2.
- Produces: `bool State::ReplayFrame(const std::vector<float>* lerped_vs, ...)` — в этой задаче без смешивания: каждая `DrawOp` исполняется со своими константами; возвращает false и причину в `Skip("interp: <причина>")`, если журнал пуст, `Target*` журнала больше нет в `targets`, или кольцо переполнено.
- `Present` при флаге и интервале 2:
  1. Исполнить обычный кадр N как сейчас, но не показывать.
  2. Скопировать источник кадра N в «удержанную» текстуру (`held`, формат `kColorFormat`, размер окна).
  3. `ReplayFrame` → источник теперь промежуточный кадр.
  4. Получить два изображения swapchain (два семафора acquire), записать present-проход для промежуточного кадра в первое и для `held` во второе, один submit, два `present` подряд.
  5. Если шаг 3 вернул false: показать только кадр N, как без флага.
- Счётчики в `native 5s`: `interp=<промежуточных> interp_skip=<пропусков>`.

- [ ] **Step 1:** реализовать шаги 1–5.
- [ ] **Step 2: Verify** на заставке с флагом: валидация чистая, `interp=` около 150 за 5 с, картинка как без флага.
- [ ] **Step 3: Verify Review Focus 2–5:** изменить размер окна во время работы (вылета нет, `interp_skip` растёт по причине), загрузка/Bink (`interp_skip` с причиной пустого журнала), свёрнутое окно (вылета нет).
- [ ] **Step 4: Commit** `feat: replay the frame and show it before the real one`
- [ ] **Step 5:** собрать zip (`out/dist/...`, `run_native.bat` и `run_native_interp.bat` с `--native_interpolate=true`) для проверки пользователем: картинка идентична, в деревне и меню 60 кадров, скорость прежняя.

### Task 5: Смешивание констант вершинного шейдера

**Files:**
- Modify: `src/render/renderer.cpp`

**Interfaces:**
- Consumes: `interp::MatchDraws`, `interp::LerpFloats`, `interp::MatchedShare` (Task 1), `ReplayFrame` (Task 4).
- Produces: `struct PrevFrame { std::vector<uint64_t> keys; std::vector<float> vs_constants; /* 1024 float на отрисовку */ uint32_t interval; bool valid; }` в `State`; заполняется в конце `Present` из журнала и кольца.
- `ReplayFrame` для отрисовки с парой кладёт в кольцо `LerpFloats(prev, cur, 1024, 0.5f)` и подменяет адрес `vs_constants`; без пары — константы N.
- Пропуск, если `MatchedShare < 0.9f` (`Skip("interp: few matches")`).

- [ ] **Step 1:** снимок `PrevFrame`, смешивание, порог.
- [ ] **Step 2: Verify** заставка с флагом: валидация чистая, в `native 5s` `match=<доля>` близко к 1.0.
- [ ] **Step 3: Commit** `feat: blend vertex constants for the in-between frame`

### Task 6: Кости и правила режима

**Files:**
- Modify: `src/render/renderer.cpp`

**Interfaces:**
- Consumes: `interp::LerpHalfs`, `interp::ShouldInterpolate` (Task 1).
- `PrevFrame` дополнительно хранит тексели 1D-слотов каждой отрисовки (формат k_16_16_16_16_FLOAT, байты как в кольце).
- `ReplayFrame` для пары с одинаковым размером 1D-текстуры берёт новый слот пула, заливает `LerpHalfs` и переписывает индекс слота в копии общих констант. Другие форматы 1D — данные кадра N.
- Решение о промежуточном кадре: `ShouldInterpolate(flag, interval кадра N, prev.valid, prev.interval)`; при смене интервала `prev.valid = false`.
- Смена плана: если у больше чем половины пар `interp::Jumped(prev c0–c3, cur c0–c3, 16, 0.5f)`, промежуточный кадр пропускается (`Skip("interp: camera jump")`).

- [ ] **Step 1:** реализовать.
- [ ] **Step 2: Verify** заставка с флагом и без: валидация чистая, бой (пользователь) без промежуточных кадров: `interp=0` при 60 Гц.
- [ ] **Step 3: Commit** `feat: blend bones and skip the in-between frame on mode changes`
- [ ] **Step 4:** zip для пользователя: деревня плавная, бег прежней скорости, персонажи без «двойников», бой как без флага, катсцены без мигания на сменах плана (в логе растёт `interp: camera jump`).
