# GEMINI.md — план работ для Gemini (сборка, подпись, тест на железе)

> Этот файл — инструкция для агента Gemini, который работает в этом репозитории на Windows-машине с WDK и на тестовом MacBookPro16,1.
> Перед работой прочитай:
> 1. [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) — как устроен драйвер (железо, протокол, тракт, стадии, лог);
> 2. последнюю датированную запись в [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — единственный авторитетный статус;
> 3. [`docs/analysis/RE_speaker_layout_and_clock.md`](docs/analysis/RE_speaker_layout_and_clock.md) — раскладка динамиков и часы T2.

---

## 0. Текущее состояние (на 2026-10-28)

* Основная ветка — **`diagnostics`** (ветка по умолчанию). CI (`.github/workflows/build.yml`) на ней зелёный: драйвер собирается с WDK (Release x64), три хост-теста проходят на MSVC, gcc и clang.
* **Этапы B и C пройдены на железе** (2026-10-28, хэш `4529472D…254E`, `docs/logs/EXCERPT_20261028_035800.md`): стадии 1–5 без START_IO, стерео-endpoint, BAR/BufferStruct, BCE probe (latency 0 + 11 кадров, safety 71), удержание транспорта, запись в BAR0. R1 по минидампам подтверждает причину BSOD (`docs/analysis/RE_minidump.md`).
* **START_IO, приём `0x222020`, часы T2, питание и звук на железе ещё не проверены:**
  * перехват `IOCTL 0x222020` от AppleUSBVHCI — исправление BSOD `0x7E` при `START_IO`, только для IRP режима ядра;
  * тег запроса 1..127 с проверкой ответа, таймаут 6 с; `SET_REMOTE_ACCESS` драйвер больше не шлёт (его делает AppleUSBVHCI);
  * **стерео-endpoint**: драйвер сам раскладывает L/R на 6 динамиков (вуферы 0/1 и 3/4, твитеры 2/5 через ФВЧ 3 кГц);
  * опережение записи 1000 кадров, которое держится по `UPDATE_TIMESTAMP`; метки до START_IO отбрасываются;
  * `IAdapterPowerManagement`: STOP_IO перед D3, без записи в BAR в D1–D3;
  * S24_LE, `'pft '`/`'ltnc'`/`'saft'`, WC-маппинг BAR0, PnP teardown.
* `tools/T2Audio-TestAgent.ps1` принимает `-BceIo` (START_IO) **только** с `-Mmio` и `-OwnerApprovedBceIo`. Этот флаг ставишь не ты: только по прямому разрешению владельца (§6).
* Твой PR imacorpoliz/t2audiowindows#1 (R1–R3) открыт; в нём ревью с замечаниями (§7).

---

## 1. Жёсткие правила (нарушать нельзя)

0. **Тишина.** Дома спят. Пока владелец явно не разрешил звук:
   * не запускай `tools/Test-WasapiTone.ps1`, музыку, системные звуки, «проверку динамиков» Windows;
   * не проверяй AppleAudio «на слух» после отката — только состояние устройства (`Get-PnpDevice`);
   * **перед любым прогоном на железе:** Mute в Windows и громкость 0.

   `tools/Test-WasapiRender.ps1` допустим: он только открывает поток (`Initialize`/`Start`/`Stop`) и сэмплов не пишет.
1. **`-BceIo` / `-OwnerApprovedBceIo` — только после того, как владелец в чате написал разрешение на этот конкретный прогон.** Не отправляй `START_IO` в обход агента: ни своим скриптом, ни через реестр (`EnableBceIo`).
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
1. Пересобери пакет из текущего `diagnostics`. Код драйвера с прогона 2026-10-28 не менялся, но обновлены `tools/Test-WasapiRender.ps1` и агент.
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

### 6.1 Стерео

После PASS этапа D, громкость Windows **1–5 %**, обычная музыка или голос. Эквализации Apple в драйвере нет: звук будет «сырой», но должен идти со всех динамиков. Запиши:
* играет ли что-нибудь;
* есть ли треск или щелчки раз в ~0,35 с (это была бы ошибка опережения);
* звучат ли обе стороны и твитеры.

Если твитеры неприятны: `Parameters\TweeterMode=0` (только вуферы) и перезапуск устройства.

### 6.2 Раскладка (по одному каналу)

Только с `-RawChannels` (**твитеры без защиты**): тихий тон **1 кГц и выше**, по одному каналу 0…5. Для этого `tools/Test-WasapiTone.ps1` нужно расширить выбором канала: сделай это отдельным PR. Запиши, какой динамик звучит. Низкие частоты на каналы 2 и 5 **не подавать**. Если порядок не совпал с таблицей в `DRIVER_GUIDE.md` §4.3, поправь `T2AUDIO_SPK_*` в `src/T2AudioSpeakerLogic.h` и тесты.

---

## 7. Задачи разработки и реверса (параллельно, без железа)

Каждая задача — отдельная ветка и PR.

1. **PR #1 (R1–R3):** исправь замечания ревью:
   * не помечать «ДОКАЗАНО» без запуска;
   * выдержки кода R3 — строго по бинарнику;
   * типы сообщений 1/2/3;
   * учесть, что `NOTIFICATION_ALIVE` тоже будит событие ответа VHCI.

   Затем **R1 по минидампам**: стек `ks!DispatchDeviceIoControl+0x15`, IRP с кодом `0x222020`, `FileObject == NULL`. Это главное условие для этапа D.
2. **Реверс (REA + Ghidra, `docs/REVERSE_PLAN.md`):**
   * нужен ли `SET_PROPERTY 'nsrt'` перед START_IO;
   * что такое `'pfta'`, `'ring'`, `'sact'`, `'term'`, `'lcdr'`, `'lcdv'`;
   * номера каналов в 6-канальной ветке цикла копирования AppleAudio (`0x14000A715`–`0x14000B2C0`) — статическое подтверждение раскладки.
3. **D4. Упаковка в CI:**
   * копировать `.sys` в `packaging/`, проставлять `DriverVer`, прогонять `InfVerif`, собирать неподписанный каталог (как `BuildT2TouchId.yml` у osaul4ik);
   * секреты в CI не добавлять.
4. **Позже:** выбор канала в `Test-WasapiTone.ps1`; аппаратная громкость (msg 28, controls); наушники (`'jack'`); микрофон; удалить устаревший hardware-MDL путь (`T2AudioCreateSpeakerMdl`, `MdlVariant`).

---

## 8. Как оформлять работу

* Коммиты небольшие, с понятным сообщением: что, зачем, чем проверено.
* Любая новая арифметика или разбор — в `src/BceProtocolLogic.h` / `src/T2AudioBufferLogic.h` / `src/T2AudioSpeakerLogic.h`, плюс тест в `tests/`. CI должен быть зелёным.
* Изменение статуса — датированная запись в `docs/CURRENT_STATE.md`. Изменение поведения — обновить `docs/DRIVER_GUIDE.md`.
* В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза».
