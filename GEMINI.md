# GEMINI.md — план работ для Gemini (сборка, подпись, тест на железе)

> Этот файл — инструкция для агента Gemini, который работает в этом репозитории на Windows-машине с WDK и на тестовом MacBookPro16,1.
> Перед работой прочитай:
> 1. [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) — как устроен драйвер (железо, протокол, тракт, стадии, лог);
> 2. последнюю датированную запись в [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — единственный авторитетный статус;
> 3. [`docs/analysis/RE_speaker_layout_and_clock.md`](docs/analysis/RE_speaker_layout_and_clock.md) — раскладка динамиков и часы T2;
> 4. [`docs/REVIEW_20261028.md`](docs/REVIEW_20261028.md) — что изменилось после этапа D, найденные проблемы и быстрые откаты.

---

## ▶ СЛЕДУЮЩИЙ ЗАПУСК — делай по шагам (обновлено 2026-10-28, после этапа E)

**Где мы:** звук на железе есть (1.0.8.0, `docs/logs/EXCERPT_20261028_143928.md`). Владелец слышит: плоско, иногда хрипит, тихо. Разбор Claude — первая запись в `docs/CURRENT_STATE.md`. Главное из него:
* щелчок в начале каждого потока из-за «переполненного» hostTime при аптайме больше 25,6 ч — исправлено в коде;
* дрейф часов T2 всего ~10 ppm, хрип не из-за него;
* вторая подозреваемая причина — задержки DPC, теперь они измеряются строкой `run summary`.

Задача сессии: **собрать новую сборку и дать владельцу послушать её самому** в режиме `-Listen`. Затем собрать логи.

**Шаг 1. Ветка и CI.**
```powershell
git fetch origin
git checkout -b gemini/listen-1 origin/diagnostics
```
GitHub → Actions: «Build» на этом коммите зелёный. Если красный — стоп, сообщи владельцу.

**Шаг 2. Сборка и подпись** по §2 (A1–A3). `DriverVer` = сегодняшняя дата, **`1.0.9.0`**. SHA256 `.sys` положи в `$h`. `.sys` и `.cat` не коммить.

**Шаг 3. Установка** по §3 B1: сначала отключи `PCI\VEN_106B&DEV_1803`, потом `pnputil /add-driver packaging\T2AudioMiniport.inf /install`. В реестре `…\T2AudioMiniport\Parameters` удали старые `OutputAttenuation`/`ClockRateTracking`, если остались с прошлой сессии: нужны значения по умолчанию.

**Шаг 4. Stage 1 (беззвучно):**
```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
```
PASS: в `t2audio_messages.txt` строка `mode parameters: … OutputAttenuation=1 (-6 dB) ClockRateTracking=0`.

**Шаг 5. Прослушивание №1 — новая сборка, настройки по умолчанию** (владелец разрешил START_IO и звук):
```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -Listen -OwnerApprovedAudible -ExpectedSysHash $h
```
Агент запустит драйвер и будет ждать Enter. Дальше:
1. Скажи владельцу выбрать динамики T2 устройством вывода и **снять Mute** на этой точке. Пробы оставляют её заглушённой.
2. Громкость поднимать постепенно, начиная с 5–10 %.
3. Владелец слушает сколько хочет: музыку, видео, паузы и повторный старт. Пусть отмечает время, когда слышит хрип.
4. Enter — драйвер выключается.

Сразу после: `.\tools\T2Audio-TestAgent.ps1 -Mode Recover`.

Из лога выпиши **все** строки `run summary: …` (по одной на каждый поток) и строки `re-aimed`. Что ожидается после исправления:
* в начале потока **один** `re-aimed`, а не два;
* `gaps>lead=0` и `skips=0` — если не 0, хрип от задержек DPC (это для Claude).

**Шаг 6. Прослушивание №2 — то же с `Parameters\ClockRateTracking=1`** (dword). Ожидается строка `T2 clock: rate=… ppb drift=… ppb lead=…`, где drift около +10 000 ppb (≈ +10 ppm), а lead держится около 1000. Если что-то странное — верни 0.

**Шаг 6b. Прослушивание №3 — DSP kaiT2en в драйвере (`Parameters\SpeakerDsp=1`).** Появилось в `diagnostics` после блока выше. Если 1.0.9.0 собран с более раннего коммита, сначала закончи шаги 5–6. Потом:
1. `git pull`, пересобери как **`1.0.10.0`**, переустанови (шаги 2–3).
2. Поставь `SpeakerDsp=1` (dword) в `Parameters`.
3. `ClockRateTracking` оставь таким, какой лучше показал шаг 6, или 0.
4. Запусти прослушивание командой из шага 5.

Ожидается:
* в логе `mode parameters: … SpeakerDsp=1`;
* звук заметно «собраннее»: глубокий бас ниже ~70 Гц срезан (это защита вуферов), середина и верх ровнее, стерео шире;
* хрипа на басу должно стать меньше, если он был от перегруза вуферов.

Затем то же с **`SpeakerDsp=2`** (добавлен виртуальный бас: бас должен ощущаться полнее, хотя ниже ~70 Гц по-прежнему срезан). Попроси владельца сравнить №1, `SpeakerDsp=1` и `SpeakerDsp=2` на той же громкости и на тех же треках. Выпиши все `run summary` (там есть `dspLimited=`). Тихо или звук «вялый»: сначала проверь, что `OutputAttenuation` = 1 или удалён, потом громкость Windows. Нелинейных блоков kaiT2en (виртуальный бас, компрессор) в драйвере пока нет.

**Шаг 7. Отчёт:**
1. `docs/logs/EXCERPT_<дата>_<время>.md` для всех прослушиваний: `mode parameters`, все `run summary`, все `re-aimed`, первые строки `rate=`, слова владельца со временем хрипов.
2. Датированная запись в `docs/CURRENT_STATE.md`: хэш, версия, результат по шагам.
3. PR из своей ветки в `diagnostics`.
4. В конце устройство на AppleAudio (`Started`, `CM_PROB_NONE`), значения в `Parameters` (`OutputAttenuation`, `ClockRateTracking`, `SpeakerDsp`) удалены.

**Если что-то не работает:** `-Mode Recover`, сохрани `capture_agent_*` и минидамп, опиши в `CURRENT_STATE.md` и передай владельцу. Драйвер наугад не чини.

---

## 0. Текущее состояние (на 2026-10-28, вечер; актуальное — в блоке выше и в `docs/CURRENT_STATE.md`)

* Основная ветка — **`diagnostics`** (ветка по умолчанию). CI (`.github/workflows/build.yml`): драйвер собирается с WDK (Release x64), три хост-теста проходят на MSVC, gcc и clang.
* **Этапы B, C и D пройдены на железе.** Последний прогон D: сборка 1.0.7.0, хэш `48F336C1…BE4`, `docs/logs/EXCERPT_20261028_043442.md`.
  * BSOD `0x7E` не возникает: `IOCTL 0x222020` перехватывается в ядре.
  * Ответы START_IO/STOP_IO принимаются (`msg=1`/`msg=3`, `status=0`).
  * Поток входит в RUN, часы T2 держат опережение 1000 кадров.
  * После STOP_IO метки времени больше не приходят.
* **Ещё не проверено на железе:** что T2 читает наше кольцо (то есть сам звук), питание (сон/D3), переполнение hostTime на большом аптайме.
* **Новое после прогона D (только исходники, на железе не проверено):**
  * `Parameters\OutputAttenuation`: защитное ослабление каждого сэмпла шагами по 6 дБ. По умолчанию 3, то есть −18 дБ; 0 выключает, максимум 16. Значение видно в логе: `mode parameters: … OutputAttenuation=3 (-18 dB)`.
  * `tools/Test-WasapiTone.ps1 -EndpointVolume 0.01..0.05`: тихий тон только на явно указанной точке, после тона Mute и громкость 0 возвращаются. Исправлена запись 32-битного PCM: раньше в 32-битный буфер писались 16-битные сэмплы.
  * `tools/T2Audio-TestAgent.ps1 -AudibleVolume <0.01..0.05> -OwnerApprovedAudible`: этап E. Сначала идёт беззвучная проба, только потом тон.
  * `Parameters\ClockRateTracking` (по умолчанию 0): подстройка скорости под часы T2 против скачков при дрейфе. Найдено симуляцией `tests/T2AudioPipelineTest.c`, см. §7 E2/E3.
  * Уборка: старые документы перенесены в `docs/archive/`. `.sys` и `.cat` больше не коммитятся (§2 A3). Пути к DbgViewCLI: `$env:T2AUDIO_DBGVIEW` или `..\DebugView\`.
  * CI компилирует C# из `Test-WasapiTone.ps1` и `Test-WasapiRender.ps1`, гоняет pipeline-тест. PSScriptAnalyzer, InfVerif и анализ кода работают в режиме «только отчёт».
* Открытых PR нет.

---

## 1. Жёсткие правила (нарушать нельзя)

0. **Тишина.** Дома спят. Пока владелец явно не разрешил звук:
   * не запускай `tools/Test-WasapiTone.ps1` (кроме проверки компиляции из §7 S1, которая звук не включает), музыку, системные звуки, «проверку динамиков» Windows;
   * не проверяй AppleAudio «на слух» после отката — только состояние устройства (`Get-PnpDevice`);
   * **перед любым прогоном на железе:** Mute в Windows и громкость 0.

   `tools/Test-WasapiRender.ps1` допустим: он только открывает поток (`Initialize`/`Start`/`Stop`) и сэмплов не пишет.
1. **`-BceIo` / `-OwnerApprovedBceIo` — только после того, как владелец в чате написал разрешение на этот конкретный прогон.** Не отправляй `START_IO` в обход агента: ни своим скриптом, ни через реестр (`EnableBceIo`).
1a. **Звук (`-AudibleVolume`, `-Listen`, `-OwnerApprovedAudible`)** — только по разрешению владельца на звук. Разрешение на START_IO звук не разрешает. Тон: громкость точки ≤ 0.25, амплитуда ≤ 0.3. В `-Listen` громкостью управляет владелец, начинать с 5–10 %. `OutputAttenuation` не ставить ниже 1 без разрешения.
2. **Не убирай гейт `TestSession`** в `src/Driver.c` и не делай автозапуск драйвера при обычной загрузке.
3. **Не уводи Mac в сон** во время сессии стадии ≥ 3 (обработка питания есть, но не проверена).
4. Стадии поднимай **по одной**: 1 → 2 → 3 → 5 без START_IO → (с разрешения) START_IO. Следующая стадия — только если предыдущая прошла все критерии.
5. После каждого прогона, особенно после сбоя: `.\tools\T2Audio-TestAgent.ps1 -Mode Recover`. Устройство в итоге должно вернуться на **AppleAudio** (Started, без ошибок).
6. Не пиши «исправлено» / «работает» в документации и коммитах без доказательства с железа (лог, дамп, хэш бинарника).
7. Не коммить сертификаты, `.pfx`, приватные ключи, полные логи ядра (`docs/logs/capture_*/` в `.gitignore`), бинарники Apple и проекты REA/Ghidra. В репозиторий кладутся только выдержки; бинарники и большие листинги — в приватный `imacorpoliz/mbp`.
8. В `diagnostics` напрямую не пушь. Работай в своей ветке от `diagnostics` (например, `gemini/hw-stage3`) и открывай PR в `diagnostics`.
9. `-RawChannels` (6 сырых каналов, **твитеры без защиты**) — только для проверки раскладки и только со звуком, разрешённым владельцем (§6.2).

---

## 2. Этап A — сборка и подпись тестового пакета

### A1. Сборка

```powershell
git fetch origin
git checkout -b gemini/hw-stage3 origin/diagnostics

msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild `
        /p:WindowsTargetPlatformVersion=10.0.26100.0     # или версия установленного SDK
# -> src\bin\Release\T2AudioMiniport.sys
```

В проекте указан `WindowsTargetPlatformVersion 10.0.28000.0`. Если такого SDK нет, передай установленный через `/p:`, как это делает CI. Вместо локальной сборки можно взять артефакт `T2AudioMiniport-unsigned-<sha>` из GitHub Actions того же коммита.

### A2. Хост-тесты (обязательно перед каждым бинарником)

```powershell
foreach ($t in 'BceProtocolLogicTest','T2AudioBufferLogicTest','T2AudioSpeakerLogicTest') {
    cl /nologo /W4 /Fe:tests\$t.exe tests\$t.c
    & "tests\$t.exe"; if ($LASTEXITCODE) { throw "$t failed" }
}
```

### A3. Пакет и подпись

1. Скопируй `src\bin\Release\T2AudioMiniport.sys` в `packaging\`.
2. **Подними `DriverVer`** в `packaging\T2AudioMiniport.inf`: сегодняшняя дата и следующая версия после последней установленной (например, `1.0.5.0`).
3. Подпиши `.sys`, пересоздай и подпиши каталог тестовым сертификатом проекта (SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`, `docs/T2AudioCert_thumbprint.txt`; он должен лежать в `Cert:\CurrentUser\My` и в доверенных):

```powershell
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"   # своя версия
cd packaging
& "$kit\signtool.exe" sign /fd SHA256 /sha1 CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE `
    /t http://timestamp.digicert.com T2AudioMiniport.sys
& "$kit\makecat.exe" t2audio.cdf                      # -> t2audiominiport.cat
& "$kit\signtool.exe" sign /fd SHA256 /sha1 CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE `
    /t http://timestamp.digicert.com t2audiominiport.cat
(Get-FileHash .\T2AudioMiniport.sys -Algorithm SHA256).Hash   # запомни: это -ExpectedSysHash
```

4. Проверь подписи: `& "$kit\signtool.exe" verify /pa /v T2AudioMiniport.sys`, то же для `.cat`.

**Готово, когда:** тесты прошли, `.sys` и `.cat` подписаны, хэш `.sys` записан.

**Не коммить `.sys` и `.cat`** (они в `.gitignore` с 2026-10-28). В репозиторий идут только `T2AudioMiniport.inf` с новым `DriverVer` и хэш `.sys` в отчёте. Подписанные пакеты, если их нужно хранить, кладутся в приватный `imacorpoliz/mbp`.

---

## 3. Этап B — прогон на железе, стадии 1 → 3 (беззвучно)

### B1. Подготовка

```powershell
.\tools\PreInstall-Check.ps1
.\tools\Install-T2AudioDriver.ps1            # только добавляет пакет в driver store
.\tools\Install-T2AudioDriver.ps1 -Bind      # привязка к устройству (может потребовать перезагрузку, Code 14)
.\tools\T2Audio-TestAgent.ps1 -Mode Status
```

Для логов нужен `DbgViewCLI.exe` (Sysinternals, см. `tools/README.md`). Mute и громкость 0 — до первого прогона.

**После обновления со старой 6-канальной сборки** Windows может держать в кэше старый формат endpoint, а новый драйвер его отклоняет. В логе это строка `Validate: 6 channels not offered`, endpoint не стартует. Лечение: «Звук → Свойства устройства → Дополнительно → 2 канала, 32 бит, 48000 Гц». Или переустанови устройство (`pnputil /remove-device <InstanceId>`, затем `pnputil /add-driver packaging\T2AudioMiniport.inf /install`).

### B2. Прогоны

```powershell
$h = '<SHA256 из A3>'
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 2 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 3 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

При BSOD или зависании на любой стадии останавливайся. После перезагрузки: `-Mode Recover`, собрать `C:\Windows\Minidump\*.dmp` (`!analyze -v`, стек), в отчёт — только это.

### B3. Критерии приёма (строки лога `T2Audio:`)

| Стадия | Должно быть |
|---|---|
| 1 | `diagnostic session active stage=1 … rawChannels=0`, `PcRegisterAdapterPowerManagement status=0x00000000`, `StartDevice SUCCESS`; WASAPI-проба открывает endpoint (формат **2 ch / 48000 / 32 bit**); без BSOD |
| 1 | `mode parameters: … TweeterMode=1` |
| 2 | `Resource[0] mapped at … (write-combining\|uncached)`, `GPR read: … signature=0x19870423`, `MapResources SUCCESS: … size=0x61800` |
| 3 | `BCE probe: 5 device(s)`, `uid="Speaker"` для `0x39` |
| 3 | `speaker stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24` |
| 3 | `speaker sample shift=8 latency=A+B frames safety=C frames` — **запиши A, B, C** |
| 3 | `BCE probe done; speaker id 0x39 stream 0x3A format=1` |
| 3 | при выключении: `PnP minor 0x02/0x04 - releasing hardware`, без BSOD и без зависания PnP |
| все | после `-Mode Recover` устройство на AppleAudio, Started |

### B4. Отчёт

Создай `docs/logs/EXCERPT_<YYYYMMDD_HHMMSS>.md`:

* хэш `.sys`, коммит, версия INF;
* команды, которые запускал;
* выдержка лога по каждой стадии (только строки `T2Audio:`);
* PASS/FAIL по каждому критерию B3, значения latency/safety;
* состояние устройства после Recover.

Добавь датированную запись в `docs/CURRENT_STATE.md`: что проверено, на каком хэше, что прошло, что нет. Закоммить и открой PR.

---

## 4. Этап C — стадии 4 и 5 без START_IO (только если B полностью PASS)

Цель: удержание открытого транспорта (стадия 4), затем запись в BAR0 (стерео → 6 каналов, WC + `sfence`) и обнуление кольца (стадия 5). Звука **не будет**: без START_IO T2 кольцо не читает.

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 4 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Критерии:

* нет BSOD;
* стадия 4: `audio path ENABLED for speaker 0x39 (bceIo=0, transport held open)`;
* стадия 5: `AllocateAudioBuffer: system … size=…` (размер кратен 8 — стерео), `SetState 3 …`, на стопе `SetState 0 … ticks=N` с N > 0;
* строк `T2 clock: re-aimed` быть **не должно**: без START_IO нет и меток;
* после Recover устройство на AppleAudio, `Started`.

Отчёт — как в B4.

---

## 5. Этап D — беззвучный START_IO (только с разрешения владельца)

Это проверка исправления BSOD и часов T2. Звука нет: Windows в Mute, движок пишет нули, кольцо перед стартом обнуляется.

Условия: B и C полностью PASS (выполнено), R1 по минидампам не противоречит причине (выполнено), и **владелец в чате разрешил этот прогон**.

Перед прогоном (обязательно):
1. Пересобери пакет из текущего `diagnostics` (порядок — блок «▶ Следующий запуск» в начале файла).
2. **Заглуши саму конечную точку T2, а не устройство по умолчанию.** Windows хранит громкость и Mute для каждой конечной точки отдельно, а новая точка T2 может стать устройством по умолчанию на 100 %. Сделай прогон `-Stage 1`: проба теперь сама ставит Mute и громкость 0 на точке T2. В `wasapi.txt` должно быть `endpoint mute=True volume=0` и `T2 endpoint muted, volume 0.`. Если там `MUTE NOT CONFIRMED`, стоп. Windows запоминает это состояние, поэтому на прогоне D точка уже будет заглушена, и проба проверит это ещё раз до `Start`.
3. «Панель управления → Звук → Звуки → Звуковая схема: Без звука» на время прогона (верни после), плюс Mute и громкость 0 на AppleAudio.

```powershell
# Mute + громкость 0 — проверь ещё раз
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Ожидаемое в логе:

```
T2Audio: SetState RUN StartIo status=0x00000000
T2Audio: START_IO sent to device 0x39
T2Audio: VHCI UPDATE_TIMESTAMP #1 dev=0x39 t2=… seed=… sample=…
T2Audio: VHCI UPDATE_TIMESTAMP #2 …                          # примерно через 347 мс
T2Audio: T2 clock: re-aimed lead by … frames (#1, …)          # 0–2 раза в начале, дальше редко
T2Audio: STOP_IO sent to device 0x39                          # на стопе
```

Критерий беззвучности: в `wasapi.txt` строка `endpoint mute=True volume=0`.

Запиши:
* шаг `sample` между метками (ожидается 16 640);
* интервал между метками;
* сколько раз сработало `re-aimed` за минуту.

Если `re-aimed` срабатывает на каждой метке — это ошибка модели часов. Остановись и приложи лог.

**BSOD** → стоп, минидамп, отчёт. Повторять нельзя, пока не разобрано.

---

### 5.1 Повтор этапа D (после исправления ответа START_IO)

Первый прогон D (2026-10-28) прошёл без BSOD, но ответ T2 на START_IO был отклонён нашей проверкой, и поток не вошёл в RUN. Проверка исправлена. Повтор — по тем же правилам и **только с новым разрешением владельца**: пересобрать, Stage 1 с подтверждением mute, затем тот же прогон D. Ожидаемое:

```
T2Audio: START_IO reply size=256 tag=… type=… dev=0x… msg=1 status=0x00000000   # запиши все поля
T2Audio: SetState RUN StartIo status=0x00000000
T2Audio: VHCI UPDATE_TIMESTAMP #1 … sample=…
T2Audio: T2 clock: re-aimed lead by … frames (#1, …)    # ожидается 1 раз в начале (~ −1900: T2 начинает читать ~40 мс после START_IO)
T2Audio: STOP_IO reply size=… msg=3 status=0x00000000
T2Audio: STOP_IO sent to device 0x39
```

Запиши:
* все поля строк `START_IO reply` и `STOP_IO reply`;
* сколько раз сработало `re-aimed` и на сколько кадров. Больше 2–3 раз за 2 секунды — ошибка, приложи лог;
* `ticks` > 0 на стопе;
* после стопа метки времени **больше не приходят**;
* аптайм на момент теста (`(Get-CimInstance Win32_OperatingSystem).LastBootUpTime`) — для проверки переполнения hostTime.

Если START_IO снова отклонён (`START_IO response invalid`), сразу после него в логе должен быть `STOP_IO`, и метки должны прекратиться.

## 6. Этап E — первый звук (только когда владелец разрешит шуметь)

### 6.0 Сборка для этапа E

Пересобери из актуальной `diagnostics` (этап A полностью, новый `DriverVer`, хэш в отчёт). Проверь, что CI зелёный: изменения в `Driver.c`, `WaveRTStream.c` и `Test-WasapiTone.ps1` (C# внутри `Add-Type`) ещё ни разу не компилировались на Windows. Если `Add-Type` падает, исправь компиляцию отдельным коммитом и опиши ошибку.

Перед звуковым прогоном сделай беззвучный повтор этапа D на новой сборке, это тоже только с разрешения владельца. В логе должна быть строка `OutputAttenuation=3 (-18 dB)`.

### 6.1 Стерео: тихий тон через агент

Только после явного разрешения владельца на **звук** (разрешение на START_IO не считается):

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo `
    -AudibleVolume 0.03 -OwnerApprovedAudible -ToneFrequency 440 -ToneSeconds 3 -ToneAmplitude 0.05
```

Как устроен прогон:
* агент сначала делает беззвучную пробу (`wasapi.txt`, Mute подтверждён);
* затем играет тон (`tone.txt`) и в конце снова ставит Mute и громкость 0;
* в драйвере сэмплы дополнительно ослаблены на 18 дБ.

Что должно быть в `tone.txt`:

```
endpoint mute=False volume=0.03
MixFormat: ch=2 rate=48000 bits=32 float=…
Start hr=0x00000000
wrote frames=… 
after run: endpoint mute=True volume=0
```

Запиши:
* слышен ли тон, с какой стороны и из каких динамиков (вуферы/твитеры);
* есть ли треск или щелчки раз в ~0,35 с (это была бы ошибка опережения);
* что в логе ядра: `re-aimed` и `ticks`, как на этапе D.

Тона нет, а в логе всё как на этапе D? Повтори с `-ToneFrequency 1000` и `Parameters\OutputAttenuation=1` (−6 дБ). Громкость точки не повышай. Если и так тихо, остановись и пиши отчёт: значит, T2 не читает кольцо, нужен реверс (§7).

Если твитеры неприятны: `Parameters\TweeterMode=0` (только вуферы) и перезапуск устройства.

Обычная музыка допустима только после PASS тона, на громкости 1–5 % и с тем же `OutputAttenuation`.

### 6.2 Раскладка (по одному каналу)

Только с `-RawChannels` (**твитеры без защиты**): тихий тон **1 кГц и выше**, по одному каналу 0…5. Агент пускает `-AudibleVolume` вместе с `-RawChannels` только с `-ToneChannel 0..5` и `-ToneFrequency >= 1000`. Сам скрипт тона тоже отказывает (`TWEETER REFUSED`), если ниже 1 кГц идёт на каналы 2/5 или на все каналы.

```powershell
foreach ($c in 0..5) {
  .\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -RawChannels `
      -AudibleVolume 0.03 -OwnerApprovedAudible -ToneChannel $c -ToneFrequency 1000 -ExpectedSysHash $h
  .\tools\T2Audio-TestAgent.ps1 -Mode Recover
  Read-Host "Канал ${c}: какой динамик звучал? (запиши)"
}
```

Запиши, какой динамик звучит. Низкие частоты на каналы 2 и 5 **не подавать**. Если порядок не совпал с таблицей в `DRIVER_GUIDE.md` §4.3, поправь `T2AUDIO_SPK_*` в `src/T2AudioSpeakerLogic.h` и тесты.

---

## 7. TODO для билдера (без железа или после этапа E)

Каждая задача — отдельная ветка от `diagnostics` и PR. Отмечай выполненное здесь же: `[x]`, дата и ссылка на PR.

**Сразу (без железа):**
- [x] **S1. Сборка новой `diagnostics` на Windows** для этапа E: драйвер, подпись, хэш (§2). Собрано 2026-10-28, DriverVer 1.0.8.0, SHA256 26A701DCA40DEF0EFBF6DC74D0D30849A83AA12D0B1E31F82436F25959ED7D7B. Звук успешно получен на железе.
- [ ] **S2. Упаковка в CI (D4).** Копировать `.sys` в `packaging/`, проставлять `DriverVer`, прогонять `InfVerif`, собирать неподписанный каталог (как `BuildT2TouchId.yml` у osaul4ik). Секреты в CI не добавлять.
- [ ] **S3. INF: значения по умолчанию.** Записать `OutputAttenuation=3` и `TweeterMode=1` в `Parameters` через `AddReg`, чтобы они были видны и редактируемы в реестре. Сейчас это только значения по умолчанию в коде.
- [ ] **S5. Driver Verifier** (стандартные флаги + IRQL checking) на стадиях 1–3, беззвучно, только с разрешения владельца. После прогона Verifier выключить (`verifier /reset`, перезагрузка) и записать результат.
- [ ] **S6. Разобрать отчёты CI с флагом report-only** (PSScriptAnalyzer, InfVerif, анализ кода WDK) в логе Actions. Первый разбор анализа кода — таблица в `docs/REVIEW_20261028.md` §1.7. Исправить найденное отдельными PR, затем убрать `continue-on-error`.
- [ ] **S4. Удалить устаревший hardware-MDL путь** (`T2AudioCreateSpeakerMdl`, `MdlVariant`, `tools/Test-MdlVariants.ps1`): буфер WaveRT давно живёт в системной памяти. Только если CI остаётся зелёным, без изменения поведения.

**Инструменты:**
- [x] **T1. Выбор канала в `Test-WasapiTone.ps1`** (`-Channel`): сделано 2026-10-28 (Claude), компилируется в CI. На железе не проверено.
- [x] **T2. `-RawChannels` в агенте вместе с тоном** (`-ToneChannel`, ≥ 1 кГц): сделано 2026-10-28 (Claude). На железе не проверено, это §6.2.

**Реверс (REA + Ghidra, `docs/REVERSE_PLAN.md`):**
- [ ] **R4.** Нужен ли `SET_PROPERTY 'nsrt'` перед START_IO.
- [ ] **R5.** Что такое `'pfta'`, `'ring'`, `'sact'`, `'term'`, `'lcdr'`, `'lcdv'`.
- [ ] **R6.** Номера каналов в 6-канальной ветке цикла копирования AppleAudio (`0x14000A715`–`0x14000B2C0`): статическое подтверждение раскладки.
- [ ] **R7.** Как AppleAudio управляет громкостью: msg 28 (`controls`), диапазон и шаги. Нужен для аппаратной громкости.

**Часы (после PASS этапа E, с разрешения владельца на звук):**
- [ ] **E2. Долгий прогон, 10 минут, `ClockRateTracking=0`** (текущее поведение). Тихая музыка или тон. Посчитать в логе строки `re-aimed` и отметить на слух щелчки и пропуски. Симуляция (`tests/T2AudioPipelineTest.c`) предсказывает при дрейфе ±80 ppm скачок раз в 1–2 минуты. Измерь реальный дрейф: разница шага `sample` (16 640) и интервала между метками в мс.
- [ ] **E3. Тот же прогон с `Parameters\ClockRateTracking=1`** (dword) и перезапуском устройства. Ожидается:
  * в логе `T2 clock: rate=… ppb drift=… ppb lead=…`, где drift — реальный дрейф T2 и стабилен;
  * `lead` держится в пределах 1000 ± 100;
  * `re-aimed` не больше 1–2 раз в начале.
  Если `rate` упирается в ±1 000 000 или `lead` уходит, верни 0 и приложи лог.

**После PASS этапа E (этапы F/G из README):**
- [ ] **F1.** Эквалайзер по профилю kaiT2en `dsp/profiles/16_1`. Сначала через Equalizer APO на стерео-точке, без изменения драйвера. Готовая заготовка: `SpeakerDSP-APO` (`bankstown.dll` + `generate_config.py`) в приватном `imacorpoliz/mbp`. Учти, что она рассчитана на 6 каналов, а точка T2 стерео: конфиг надо переделать под стерео.
- [ ] **F2.** Лимитер/DRC в драйвере: целочисленный, в `T2AudioSpeakerLogic.h` с тестами, вместо грубого `OutputAttenuation`.
- [ ] **G1.** Аппаратная громкость (после R7).
- [ ] **G2.** Наушники: события `'jack'`.
- [ ] **G3.** Микрофон (`0x21 Digital Mic` / `0x44 Codec Input`).
- [ ] **G4.** Обычный автозапуск без `TestSession` — **только по решению владельца**, после проверки сна/D3 на железе (правило 2 §1 до тех пор действует).

---

## 8. Как оформлять работу

* Коммиты небольшие, с понятным сообщением: что, зачем, чем проверено.
* Любая новая арифметика или разбор — в `src/BceProtocolLogic.h` / `src/T2AudioBufferLogic.h` / `src/T2AudioSpeakerLogic.h`, плюс тест в `tests/`. CI должен быть зелёным.
* Изменение статуса — датированная запись в `docs/CURRENT_STATE.md`. Изменение поведения — обновить `docs/DRIVER_GUIDE.md`.
* В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза».
