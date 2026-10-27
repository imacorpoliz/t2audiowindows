# T2AudioPort — Apple T2 Audio Driver for Windows (WaveRT)

[![Build & Test](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml/badge.svg?branch=diagnostics)](https://github.com/imacorpoliz/t2audiowindows/actions/workflows/build.yml)
[![Status](https://img.shields.io/badge/Hardware%20Status-Stage%20D%20PASS%20(MacBookPro16%2C1)-brightgreen)](#hardware-verification-status)
[![Driver](https://img.shields.io/badge/Architecture-WaveRT%20%2F%20PortCls-blue)](#how-it-works)

> **Current Status (October 2026): Stage D verified on physical hardware.**  
> * **BSOD 0x7E is PROVEN FIXED:** Kernel-mode interception of `IOCTL 0x222020` prevents NULL-dereferences in `ks.sys`.
> * **Hardware I/O Proven:** `START_IO` starts DMA playback on bridgeOS; `UPDATE_TIMESTAMP` clocks lock with a stable 1000-frame lead (step: 16,640 frames = 346.66 ms); `STOP_IO` cleanly halts DMA.
> * **Endpoint:** Windows sees a standard **Stereo 48 kHz / 32-bit** endpoint. The driver unpacks stereo across all 6 physical speakers (woofers + high-passed tweeters).
> * **Safety Mode:** The driver runs only inside an explicit, volatile `TestSession` to protect system stability.

📘 **Полная документация на русском языке: [`docs/DRIVER_GUIDE.md`](docs/DRIVER_GUIDE.md)**  
📋 **Текущий авторитетный статус разработки: [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md)**

---

## What Remains for "macOS-Quality" Sound (Что осталось до звука как на macOS)

MacBook Pro славится лучшим звуком среди ноутбуков не только за счёт динамиков, но и за счёт сложнейшего цифрового сигнального процессора (DSP) в macOS. Достижение звука «как на Mac» разделено на понятные этапы:

```
  [Этап D: ПРОЙДЕН] ──► [Этап E: СЛЕДУЮЩИЙ ШАГ] ──► [Этап F: DSP & Бас] ──► [Этап G: Daily Driver]
   START_IO & Часы        Первый сырой звук         Фирменный эквалайзер      Автозапуск, Jack,
    (доказано на           (2–5% громкости,          Apple, лимитер баса        микрофоны, громкость
       железе)              вуферы + твитеры)            и защита твитеров            без агента
```

### 1. Первый физический звук (Этап E — готовность 99%, следующий шаг)
* **Статус:** Вся аппаратная цепочка (DMA, кольцо BAR0, прерывания, часы, протокол BCE) уже проверена и работает.
* **Что нужно:** Снять программное заглушение (Mute) и запустить тестовый поток на громкости **2–5%**. Звук физически пойдёт из динамиков.

### 2. Фирменный эквалайзер и глубокий бас (Acoustic EQ & Bass Tuning)
* **Почему на Windows звук пока «сырой»:** В динамиках MacBook Pro вуферы механически малы. Apple в macOS применяет глубокую эквализацию корпуса: сильный подъём частот ниже 100 Гц и сглаживание паразитных резонансов алюминия.
* **Что нужно:** Загрузить профиль АЧХ (из открытых замеров [kaiT2en](https://github.com/kaiT2en/KaiT2en-Fedora) `dsp/profiles/16_1`). Это делается либо через **Equalizer APO** прямо в Windows поверх стерео-endpoint'а, либо через встроенный фильтр в драйвере.

### 3. Защита динамиков и лимитер (Peak Limiter & Dynamic Range Control)
* **Зачем нужно:** На macOS при громкости выше 30% работает предиктивный лимитер (Lookahead Limiter), не позволяющий диффузорам вуферов превысить максимальный ход на басах и сберегающий твитеры.
* **Что нужно:** Программный компрессор/лимитер в тракте до записи в BAR0, предотвращающий клиппинг при 100% громкости Windows.

### 4. Пространственная стереобаза (Crosstalk Cancellation / XTC)
* **Особенность macOS:** Звук кажется исходящим «из-за пределов экрана». Это достигается алгоритмом взаимного вычитания межканального просачивания (XTC).
* **Что нужно:** Добавить матрицу XTC-фильтрации в стерео-раскладку драйвера.

### 5. Повседневный драйвер (Daily Driver Integration)
* **Автозапуск без тестов:** Снятие ключа `TestSession`, чтобы драйвер стартовал автоматически при загрузке Windows без тестовых скриптов.
* **Аппаратная громкость (Hardware Volume):** Реализация управления усилителями через команды BCE (msg 28) для сохранения полного 24-битного динамического диапазона на любой громкости.
* **Разъём 3.5 мм (Headphone Jack):** Детекция подключения наушников через сообщения `'jack'` и переключение тракта.
* **Студийные микрофоны:** Поддержка 3-микрофонного массива (`0x21 Digital Mic` / `0x44 Codec Input`).

---

## Архитектура аудиотракта (How It Works)

```
 Windows Audio Engine (WASAPI)
        │  32-bit PCM, stereo, 48 kHz (WAVEFORMATEXTENSIBLE)
        ▼
 WaveRT Cyclic Buffer (System RAM)
        │  Copy DPC: отслеживает consumed cursor
        │  Конвертация: 32-bit float/int ──► S24_LE (сдвиг >> 8)
        │  Раскладка:
        │    L ──► Woofers [0, 1] + Tweeter [2] (4th-order Butterworth HPF 3 kHz)
        │    R ──► Woofers [3, 4] + Tweeter [5] (4th-order Butterworth HPF 3 kHz)
        │  Синхронизация: удержание опережения 1000 кадров по T2 UPDATE_TIMESTAMP
        ▼
 BAR0 Speaker Ring (399,360 байт = 16,640 кадров @ 48 kHz, Write-Combining + sfence)
        │  bridgeOS непрерывно считывает кольцо по DMA
        ▼
 Apple T2 Audio Subsystem ──► 6 Физических Динамиков MacBook Pro
```

### Ключевые компоненты:
1. **Обнаружение буферов (`src/Device.c`):** Поиск конфигурационного BAR по сигнатуре GPR `0x19870423`, парсинг `BufferStruct`, нахождение буфера `"Speaker"` (смещение `0x12c000`, размер `0x61800`). Отображение памяти как Write-Combining.
2. **Транспорт управления BCE (`src/BceTransport.c`):** Отправка команд управления через `\Device\AppleUSBVHCI` (`IOCTL 0x222018`). Перехват входящих сообщений bridgeOS (`IOCTL 0x222020`) прямо в ядре без падений в `ks.sys`.
3. **Часы и синхронизация (`src/WaveRTStream.c`):** Чип T2 выдаёт метку `UPDATE_TIMESTAMP` каждые 16 640 кадров (ровно 346.66 мс на круг). Драйвер удерживает опережение записи 1000 кадров с компенсацией стартовой задержки T2.

---

## Статус проверки на железе (Hardware Status)

Все тесты проводятся на реальном **MacBookPro16,1** (`PCI\VEN_106B&DEV_1803`):

| Стадия | Режим | Назначение | Результат | Ключевые показатели |
|---|---|---|:---:|---|
| **Хост-тесты** | Offline | Логика протокола, буфера и кроссовера | **PASS** | 0 ошибок в 3 тест-сьютах (MSVC, GCC, Clang) |
| **Stage 1** | Hardware | Проверка RAM и регистрация WaveRT | **PASS** | Стерео 48 кГц / 32 бит, WASAPI-проба OK, Mute подтверждён |
| **Stage 2** | Hardware | Отображение BAR0 и чтение GPR | **PASS** | BAR0 WC, GPR `0x19870423`, буфер `0x12c000` (размер `0x61800`) |
| **Stage 3** | Hardware | Обнаружение BCE устройств и формата | **PASS** | 5 устройств найдено; Stream `0x3A` S24_LE 6ch; задержка $0+11$ кадров, safety $71$ |
| **Stage 4** | Hardware | Удержание открытого транспорта | **PASS** | Транспорт удерживается во время открытого потока |
| **Stage 5** | Hardware | MMIO копирование в BAR0 | **PASS** | Кольцо обнуляется, DPC активен, `ticks=1006` |
| **Stage D** | Hardware | **Реальный START_IO и аппаратные часы** | **PASS** | **BSOD 0x7E побеждён!** Часы T2 залочены (16 640 кадров / 346.66 мс), чистый `STOP_IO` |
| **Recovery** | Hardware | Возврат на оригинальный драйвер | **PASS** | Чистый возврат на `AppleAudio.sys` (`oem16.inf`, Started, OK) |

Подробные протоколы аппаратных тестов: [`docs/logs/EXCERPT_20261028_043442.md`](docs/logs/EXCERPT_20261028_043442.md).

---

## Сборка и тестирование

### 1. Сборка драйвера
Необходим Visual Studio 2022+ и Windows Driver Kit (WDK):
```powershell
# Сборка Release x64
msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild

# Запуск модульных тестов
cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe tests\BceProtocolLogicTest.c && tests\BceProtocolLogicTest.exe
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c && tests\T2AudioBufferLogicTest.exe
cl /nologo /W4 /Fe:tests\T2AudioSpeakerLogicTest.exe tests\T2AudioSpeakerLogicTest.c && tests\T2AudioSpeakerLogicTest.exe
```

### 2. Подпись пакета
```powershell
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64"
cd packaging
& "$kit\signtool.exe" sign /fd SHA256 /sha1 <THUMBPRINT> /t http://timestamp.digicert.com T2AudioMiniport.sys
& "$kit\makecat.exe" t2audio.cdf
& "$kit\signtool.exe" sign /fd SHA256 /sha1 <THUMBPRINT> /t http://timestamp.digicert.com t2audiominiport.cat
cd ..
```

### 3. Тестирование через TestAgent
```powershell
# Проверка готовности (RAM only)
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash <SHA256>

# Прогон с аппаратным выводом (без звука, кольцо обнуляется)
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -ExpectedSysHash <SHA256>

# Безопасный возврат на официальный AppleAudio.sys
.\tools\T2Audio-TestAgent.ps1 -Mode Recover
```

---

## Благодарности и используемые наработки

* **[kaiT2en](https://github.com/kaiT2en/KaiT2en-Fedora)** (`modules/t2bce_audio`) — эталонная Linux-реализация и reverse engineering протокола BCE, структуры BufferStruct и раскладки динамиков MacBookPro16,1.
* **[kekrby](https://github.com/kekrby/linux-t2)** — первопроходец реверса аудиоподсистемы чипа Apple T2 в Linux.
* **[osaul4ik](https://github.com/osaul4ik/apple-t2-touchid-windows)** — подход к CI и сборке WDK-драйверов на GitHub Actions.
* **Apple Inc.** — разработчик железа Apple T2 и оригинальных драйверов Boot Camp (`AppleAudio.sys`, `AppleUSBVHCI.sys`).
