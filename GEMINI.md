# GEMINI.md — план работ для Gemini (сборка, подпись, тест на железе)

> Этот файл — инструкция для агента Gemini, который работает в этом репозитории на Windows-машине с WDK и на тестовом MacBookPro16,1.
> Перед работой прочитай:
> 1. [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md) — как устроен драйвер (железо, протокол, тракт, стадии, лог);
> 2. последнюю датированную запись в [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) — единственный авторитетный статус.

---

## 0. Текущее состояние (на 2026-10-28)

* Основная ветка: **`diagnostics`** (ветка по умолчанию). CI (`.github/workflows/build.yml`) на ней зелёный: драйвер собирается с WDK (Release x64), хост-тесты проходят на MSVC, gcc и clang.
* Последние изменения в коде **ни разу не запускались на железе**:
  * копирование с «потреблённым» курсором и опережением 15 мс;
  * конверсия сэмплов в S24_LE (`>> 8`);
  * чтение формата потока `'pft '`, latency и safety offset у T2;
  * write-combining для BAR0;
  * PnP teardown и `SET_REMOTE_ACCESS(OFF)`.
* **`START_IO` роняет систему (BSOD `0x7E`).** Это не исправлено. `tools/T2Audio-TestAgent.ps1` отказывается запускать `-BceIo` — так и должно оставаться.

---

## 1. Жёсткие правила (нарушать нельзя)

1. **Никогда не запускай `-BceIo`** и не обходи блокировку в агенте. Не отправляй `START_IO` никаким другим способом: ни своим скриптом, ни через реестр (`EnableBceIo`).
2. **Не убирай гейт `TestSession`** в `src/Driver.c` и не делай автозапуск драйвера при обычной загрузке.
3. **Не уводи Mac в сон** во время сессии стадии ≥ 3 с установленным T2AudioMiniport.
4. Стадии поднимай **по одной**: 1 → 2 → 3. Стадия 5 (`-Mmio`) — только после того, как лог стадии 3 прошёл все критерии из §3.3.
5. После каждого прогона, особенно после сбоя: `.\tools\T2Audio-TestAgent.ps1 -Mode Recover`. Устройство в итоге должно вернуться на **AppleAudio** (Started, без ошибок).
6. Не пиши «исправлено» / «работает» в документации и коммитах без доказательства с железа (лог, дамп, хэш бинаря).
7. Не коммить сертификаты, `.pfx`, приватные ключи, полные логи ядра (`docs/logs/capture_*/` в `.gitignore`). В репозиторий кладутся только выдержки.
8. В `diagnostics` напрямую не пушь. Работай в своей ветке от `diagnostics` (например, `gemini/hw-stage3`) и открывай PR в `diagnostics`.

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

В проекте указан `WindowsTargetPlatformVersion 10.0.28000.0`. Если такого SDK нет, передай установленный через `/p:`, как это делает CI. Можно вместо локальной сборки взять артефакт `T2AudioMiniport-unsigned-<sha>` из GitHub Actions.

### A2. Хост-тесты (обязательно перед каждым бинарём)

```powershell
cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe   tests\BceProtocolLogicTest.c
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c
tests\BceProtocolLogicTest.exe;   if ($LASTEXITCODE) { throw "tests failed" }
tests\T2AudioBufferLogicTest.exe; if ($LASTEXITCODE) { throw "tests failed" }
```

### A3. Пакет и подпись

1. Скопируй `src\bin\Release\T2AudioMiniport.sys` в `packaging\`.
2. **Подними `DriverVer`** в `packaging\T2AudioMiniport.inf`: сегодняшняя дата и следующая версия после `1.0.3.15`, например `1.0.4.0`.
3. Подпиши `.sys`, пересоздай и подпиши каталог тестовым сертификатом проекта (SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE`, `docs/T2AudioCert_thumbprint.txt`; должен лежать в `Cert:\CurrentUser\My` и в доверенных):

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

4. Проверь подписи: `& "$kit\signtool.exe" verify /pa /v T2AudioMiniport.sys` и то же для `.cat`.

**Готово, когда:** тесты прошли, `.sys` и `.cat` подписаны, хэш `.sys` записан.

---

## 3. Этап B — прогон на железе, стадии 1 → 3

### B1. Подготовка

```powershell
.\tools\PreInstall-Check.ps1
.\tools\Install-T2AudioDriver.ps1            # только добавляет пакет в driver store
.\tools\Install-T2AudioDriver.ps1 -Bind      # привязка к устройству (может потребовать перезагрузку, Code 14)
.\tools\T2Audio-TestAgent.ps1 -Mode Status
```

Для логов нужен `DbgViewCLI.exe` (Sysinternals, см. `tools/README.md`).

### B2. Прогоны

```powershell
$h = '<SHA256 из A3>'
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 2 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 3 -ExpectedSysHash $h
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Если на любой стадии случится BSOD или зависание, останавливайся. После перезагрузки выполни `-Mode Recover`, собери `C:\Windows\Minidump\*.dmp` (`!analyze -v`, стек), и в отчёт пойдёт только это.

### B3. Критерии приёма (строки лога `T2Audio:`)

| Стадия | Должно быть |
|---|---|
| 1 | `diagnostic session active stage=1`, `StartDevice SUCCESS`, WASAPI-проба открывает endpoint, без BSOD |
| 2 | `Resource[0] mapped at … (write-combining\|uncached)`, `GPR read: … signature=0x19870423`, `MapResources SUCCESS: … size=0x61800` |
| 3 | `BCE probe: 5 device(s)`, `uid="Speaker"` для `0x39` |
| 3 | `speaker stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24` |
| 3 | `speaker sample shift=8 latency=A+B frames safety=C frames` — **запиши A, B, C** |
| 3 | `BCE probe done; speaker id 0x39 stream 0x3A format=1` |
| 3 | при выключении: `PnP minor 0x02/0x04 - releasing hardware`, без BSOD и без зависания PnP |
| все | после `-Mode Recover` устройство на AppleAudio, Started |

### B4. Отчёт

Создай `docs/logs/EXCERPT_<YYYYMMDD_HHMMSS>.md` с:

* хэшем `.sys`, коммитом, версией INF;
* командами, которые запускал;
* выдержкой лога по каждой стадии (только строки `T2Audio:`);
* итогом по каждому критерию из B3 (PASS/FAIL), значениями latency/safety;
* состоянием устройства после Recover.

Добавь датированную запись в `docs/CURRENT_STATE.md`: что проверено, на каком хэше, что прошло, что нет. Закоммить и открой PR.

---

## 4. Этап C — стадия 5 без START_IO (только если B полностью PASS)

Цель: убедиться, что запись в BAR0 (конверсия + WC + `sfence`) и обнуление кольца не роняют систему. Звука **не будет** — `START_IO` заблокирован, и это ожидаемо.

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -ExpectedSysHash $h
# во время прогона: .\tools\Test-WasapiTone.ps1 (тон на T2-endpoint)
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

Критерии: нет BSOD; лог `AllocateAudioBuffer: system … size=…`, `SetState 3 …`, на стопе `SetState 0 … ticks=N` с N > 0. После Recover AppleAudio работает и **звук через него нормальный**: запись в кольцо не должна ломать штатный тракт. Отчёт — как в B4.

---

## 5. Этап D — оставшиеся задачи разработки (по приоритету)

Каждая задача — отдельная ветка и PR. Код сначала пишется и тестируется на хосте, железо — только по этапам A–C. Все новые доступы к железу — за гейтом стадии.

### D1. Корневая причина BSOD `0x7E` при `START_IO` (блокер всего звука)

* Известно (см. `CURRENT_STATE.md` 2026-10-08): стек `AppleUSBVHCI+0x259a → ksthunk → ks!DispatchDeviceIoControl+0x15`, чтение `NULL+0x18` (`FileObject->FsContext` у **перенаправленного** IRP). Падали оба варианта `FileObject`.
* Что сделать **офлайн** (без железа):
  1. В WinDbg на минидампах `100826-15859-01.dmp` и `100826-23546-01.dmp`: `!irp`, `kv`, `.frame`. Выяснить, какой IRP строит AppleUSBVHCI и чей это device stack.
  2. Дизассемблировать `AppleUSBVHCI.sys` вокруг `+0x259a`: куда и при каком условии он перенаправляет IOCTL в `ksthunk`. Отчёты-заготовки: `docs/analysis/AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md`, `docs/analysis/IOCTL_222008_CHAIN.md`.
  3. Сравнить с полной последовательностью `AppleAudio.sys` до первого `START_IO`. Шлёт ли он `NOTIFICATION_ALIVE` (100) с версиями 1/3, как kaiT2en (`t2audio_init_cmd`)? Открывает ли он сначала какой-то другой IOCTL (`0x222004`–`0x222014`) — регистрацию получателя уведомлений? Возможно, AppleUSBVHCI перенаправляет ответ/уведомление на `START_IO` в зарегистрированный KS-объект, а у нас его нет.
* Результат: отчёт `docs/analysis/START_IO_BSOD_ROOT_CAUSE.md` с доказательствами и предлагаемым изменением. **Включать `-BceIo` нельзя, пока владелец проекта не одобрит отчёт.**

### D2. Путь приёма уведомлений T2 → хост

* Нужен для `NOTIFICATION_ALIVE`, `UPDATE_TIMESTAMP` (аппаратная позиция) и `PROPERTY_CHANGED` (jack). Скорее всего, тот же механизм, что в D1.3.
* Когда путь известен: добавить чистый разборщик `UPDATE_TIMESTAMP` в `src/BceProtocolLogic.h` (sampleTime — IEEE double, см. `t2audio_decode_sample_time` в kaiT2en `timing.h`) с тестами. Затем заменить QPC-экстраполяцию в `T2AudioStreamAdvanceLocked` на позицию от T2.

### D3. Питание (сон/пробуждение)

* Как в kaiT2en (`t2audio_quiesce`, `t2audio_suspend`, `t2audio_reset_buffers`):
  * перед сном (`IRP_MN_QUERY_POWER` / `SET_POWER` для S1–S4): остановить I/O, `SET_REMOTE_ACCESS(OFF)`;
  * записать sleep marker `0x19770525` в GPR `+0x10` (config BAR сейчас смаплен read-only — нужен отдельный R/W-маппинг одного регистра);
  * при пробуждении: проверить marker, при необходимости перечитать `BufferStruct`, `SET_REMOTE_ACCESS(ON)`, сбросить потоки.
* В PortCls: `IAdapterPowerManagement` (`PcRegisterAdapterPowerManagement`) или хук `IRP_MJ_POWER` перед `PcDispatchIrp`.

### D4. Упаковка в CI

* Добавить в `build.yml` шаг: копировать `.sys` в `packaging/`, проставлять `DriverVer` (дата + версия из номера сборки, как в `BuildT2TouchId.yml` у osaul4ik), прогонять `InfVerif`, собирать неподписанный каталог. Подпись — только локально; секреты в CI не добавлять без владельца.
* Привести `WindowsTargetPlatformVersion` в `src/T2AudioMiniport.vcxproj` к реально доступной версии SDK (сейчас `10.0.28000.0`, CI переопределяет на `10.0.26100.0`).

### D5. Раскладка каналов и звук (после D1)

* Тон по каждому из 6 каналов (`tools/Test-WasapiTone.ps1`, расширить выбором канала). Записать, какой физический динамик звучит. Сверить с 5.1-маской и DSP-заметками (`docs/planning/CURRENT_STATE.md`, `kait2en/dsp/`).
* Если нужно — переставлять каналы при копировании в `T2AudioConvertSamples` (чистая функция, тесты).

### D6. Остальное

* Наушники (`Codec Output`, свойство `'jack'` + property listener — требует D2), микрофон (`Digital Mic`, input-потоки, `SET_INPUT_STREAM_ADDRESS_RANGES`).
* Убрать устаревший hardware-MDL путь (`T2AudioCreateSpeakerMdl`, `MdlVariant`), если он окончательно не нужен.

---

## 6. Как оформлять работу

* Коммиты небольшие, с понятным сообщением: что, зачем, чем проверено.
* Любая новая арифметика или разбор — в `src/BceProtocolLogic.h` / `src/T2AudioBufferLogic.h` плюс тест в `tests/`. CI должен быть зелёным.
* Изменение статуса — датированная запись в `docs/CURRENT_STATE.md`. Изменение поведения — обновить `docs/DRIVER_GUIDE.md`.
* В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза».
