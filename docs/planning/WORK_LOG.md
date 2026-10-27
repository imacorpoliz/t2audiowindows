# WORK_LOG — AppleAudio.sys / T2 Audio Static Analysis

**Проект**: MBP 16,1 (2019), Apple T2, Windows 11 — 6-канальный нативный аудиодоступ.
**Ограничения (действуют на все работы)**: только статический анализ; драйвер не устанавливать/не менять; MMIO не изменять; команды управления потоком (START_IO/STOP_IO и пр.) не запускать.

---

## 1. Восстановление состояния (этот аудит)

**Методы**: `git status` / `git log` (результат: каталог НЕ является git-репозиторием — git-следы недоступны), листинг файлов с временными метками, чтение всех отчётов, проверка SHA256 бинарника, проверка инструментов.

### 1.1 Git
- `git status` → `fatal: not a git repository` (рабочая директория `C:\Users\othysa\Desktop\mbp`, выполнения 2026-10-05).
- Следствие: git diff/status восстановить **невозможно**. Изменения отслеживались только по временным меткам файлов.

### 1.2 Верификация бинарника (выполнено при аудите)
```
Get-FileHash MBP-Audio-driver\AppleAudio.sys -Algorithm SHA256
→ CD8B81FABC9BE6127A082DA22C4CC046A4D46E0225EB6BAF6D27E0DC9862A150
→ 122752 bytes
```
Совпадает с зафиксированным в NOTES.md:12–15 и FINAL_REANALYSIS_REPORT.md:13. Это правильный AppleAudio.sys 6.1.8100.4.

### 1.3 Инструменты (проверены)
- `py -c "import pefile, capstone"` → OK (2026-10-05). Скрипты `disasm_func.py`, `disasm2.py`, `scan_global_fields.py`, `Verify_RVA.ps1` работоспособны.
- `MBP-Audio-driver.zip`, `AppleUSBVHCI.zip` — архивы исходных бинарников; каталог `AppleUSBVHCI-extracted` — распаковка.

---

## 2. Хронология работ по временным меткам файлов

### Фаза 0 — Пробы на целевом железе (HARDWARE-TESTED, до 09:00 04.10)
Источники: `MBP-Audio-Probe-v2\MBP-Audio-report-20261004-085054.txt`, `MBP-T2-Native-Probe\...\MBP-T2-native-report-20261004-090031.txt` (файлы существуют, содержимое пересказано в NOTES.md:32–73).

- KS-проба: WaveSpeaker публикует stereo PCM16 48 kHz; 6-канальные форматы отклонены (Win32 1169) — NOTES.md:33–38.
- T2-нативная проба: IOCTL 0x222018 через `\\.\AppleUSBVHCI`; команды 101/26/7 работают; device Speaker = 0x39, stream 0x3A, формат 48000 Hz / 6 ch / 'lpcm' / bytes_per_frame=24 / bits=24 / interleaved — NOTES.md:41–73.

### Фаза 0.5 — Создание StreamControlTest (20:38)
- Создано: `MBP-Stream-Control-Test\StreamControlTest.cs` (10440 B), `Program.cs`, `README.txt`, `.csproj`.
- **Не скомпилирован и НЕ запущен** (сознательно, по ограничениям безопасности; NOTES.md:245, 316–319).

### Фаза 1 — Документация и первый статический анализ (19:59–21:40)
Созданы (в порядке меток):
- `T2_Audio_Protocol_Analysis.md` (19:59), `T2_Windows_Implementation_Guide.md` (20:01) — перенос знаний из kait2en.
- `CURRENT_STATUS.md` (20:40), `СВОДКА_РАБОТЫ.md` (20:41) — статусы и планы.
- `AppleUSBVHCI_IOCTL_Analysis.md` (21:18), `AppleAudio_Message_Path.md` (21:26) — статический разбор ioctl-хендлеров AppleUSBVHCI (глобальный объект @ RVA 0x27488, zero в статике) и функции отправки сообщений AppleAudio (IOCTL 0x222018 @ RVA 0x4A60).
- `NOTES.md` (21:40) — основной лог: хеши, классификация доказательств, протокол T2Request/T2Reply/Property (исправленные структуры: value_size @ abs 41, value @ abs 49 — NOTES.md:190–198).

### Фаза 2 — Разбор PE и MmMapIoSpaceEx (22:04–23:11)
- `BUFFER_PATH_CORRECTIONS.md` (22:04): первый независимый пересчёт. Тогда ошибочно заключено, что вызовов MmMapIoSpaceEx нет (ошибка преобразования RVA→file offset — применялось смещение .text ко всем секциям). Признаётся ошибкой в FINAL_REANALYSIS_REPORT.md:428–432.
- Созданы скрипты: `Verify_RVA.ps1` (22:21), `disasm_func.py` (22:24), `scan_global_fields.py` (22:48), `disasm2.py` (22:40) — воспроизводимая проверка.
- `Linux_Windows_Structure_Mapping.md` (23:06), `FINAL_REANALYSIS_REPORT.md` (23:06): **исправление** — правильная таблица секций (FINAL_REANALYSIS_REPORT.md:27–36), полное сканирование EXEC-секций, найдены **ровно 2 вызова MmMapIoSpaceEx** (RVA 0x17204, 0x1721C, секция PAGE, функция 0x16FF0, IAT 0xE058) — FINAL_REANALYSIS_REPORT.md:84–91, подтверждено capstone (строки 506–510).
- `BUFFER_PATH_ANALYSIS.md` (23:09), `VERIFICATION_SUMMARY.md` (23:11) — сводка: mapped-адреса сохраняются в глобальный объект 0x14690 (+0x28 = BAR1, +0x30 = BAR2+0xC000), функция 0x4700 читает метаданные BufferStruct (magic 0x19870423).

### Фаза 3 — BufferStruct и playback (23:24–23:48)
- `Function_46F8_Analysis.md` (23:24) и `AppleAudio_BufferRegistration_Report.md` (23:32): функция 0x46F8–0x48AF — регистрация input-stream буферов в BufferStruct: поиск устройства по имени через `_strnicmp` (0x476F→0x4CC0), запись address/size (0x480A/0x4816), num_buffers @ +0x94, active @ +0x90.
- `AppleAudio_Playback_Path_Report.md` (23:48): разбор AllocateOutputBuffer 0x1C240 (vtable slot +0xB8), GetOutputBufferFromBS 0x9D74, Start/Stop handler 0xB470 (memset при остановке через 0xCD80 — доказывает реальность [stream+0xA0] как PCM pointer), вывод о zero-copy.

### Фаза 4 — Текущая сессия (2026-10-05)
- Дизассемблированы (выводы ниже в §4): 0x1C240–0x1C4BA, 0x9D74–0x9E8D, 0xCE80, 0xB300–0xB462, 0xA44C–0xA8A3, 0xC550–0xC8DC. Результаты дизассемблирования **не были записаны в файл** — восстановлены только в этом логе из проверки командой (см. §5, «не восстановлено»).
- Создан `WORK_LOG.md` (этот файл).

---

## 3. Подтверждённые факты (с источниками)

### 3.1 Оборудование и протокол (HARDWARE-TESTED)
| Факт | Источник |
|---|---|
| WaveSpeaker: только stereo PCM16 48 kHz; 6ch → error 1169 | NOTES.md:33–38, отчёт MBP-Audio-Probe-v2 |
| IOCTL 0x222018, команды 101/26/7 работают | NOTES.md:41–47, отчёт MBP-T2-Native-Probe |
| Speaker 0x39 / stream 0x3A; 48 kHz, 6ch, 'lpcm', 24 B/frame, bits 24, interleaved | NOTES.md:57–73 |

### 3.2 Static: AppleAudio.sys — память и буферы
| Факт | RVA/инструкция | Источник |
|---|---|---|
| 2 вызова MmMapIoSpaceEx | 0x17204, 0x1721C (PAGE, func 0x16FF0), IAT 0xE058 | FINAL_REANALYSIS_REPORT.md:84–91, 506–510; `disasm2.py dis 16FF0 172FE` |
| BAR1 map → [rbx+0x68] (cache 0x404), BAR2 → [rbx+0x70] (cache 0x204) | 0x1721C→0x17222; 0x17204→0x1720A | FINAL_REANALYSIS_REPORT.md:196–212 |
| Копия в глобал 0x14690: +0x28=BAR1, +0x30=BAR2+0xC000 | 0x17231–0x17247 → func 0x45F0 | FINAL_REANALYSIS_REPORT.md:222–235 |
| BufferStruct: magic 0x19870423 @+4, version @+0, bs_offset @+8 | 0x4728/0x473B/0x4744 | Function_46F8_Analysis.md:24–36; 0x9D94 cmp magic (дизассембл. 0x9D74) |
| Device stride 0xBDEC, devices @ bs+0x10, num_devices @ [rbx+0xC] | 0x475B, 0x474D | Function_46F8_Analysis.md:44–58 |
| Регистрация input буферов: address @ dev+idx*6*8+0x98, size @ +0xA0; num_buffers @ stream+0x94; active @ stream+0x90 | 0x480A/0x4816/0x4866/0x486D | AppleAudio_BufferRegistration_Report.md:127–148 |
| Output: 0x9D74 читает [dev + idx*0xBDEC + 0x5F0C]=address, +0x5F14=size (ветка version≥2); 0x5F0C = 0x10 + 0x5EF8(output_streams) + 0x04 | 0x9E78, 0x9E83 | **Дизассемблирование 2026-10-05** (`disasm2.py dis 9D74 9EE0`); совпадает с Playback_Report.md:57–65 после исправления адресов (см. §3.4) |
| 0x9D74 ветка version 1 (input): buffer entries stride 0x110, запись в [rsp+0x50]/[rsp+0x58] | 0x9DBE–0x9E20 | Дизассемблирование 2026-10-05 |

### 3.3 Static: AllocateOutputBuffer 0x1C240–0x1C4BA (vtable slot +0xB8)
Проверено дизассемблированием 2026-10-05 (`py disasm2.py dis 1C240 1C4BA`). Исправляет Playback_Report.md:35–48 (там адреса инструкций были смещены):

| Что | RVA | Примечание |
|---|---|---|
| Валидация: r8d≠0, r8d ≤ word [stream+0x150]+0xC, r8d % ebx == 0 | 0x1C269–0x1C297 | RDX=frame alignment, R8=size |
| Округление ebp = size − (size % word[[rsi+0x150]+0xC]) | 0x1C29F–0x1C2A4 | ebp = выровненный размер |
| Косвенный вызов vtable+0x18 → результат в R12 (buffer handle) | 0x1C2FB, R12@0x1C301 | через [ [rsi+0x40]→vtable ] |
| Второй косвенный вызов vtable+0x28 → результат в RAX → **[RSI+0x90]** | 0x1C324, 0x1C332 | |
| **[RSI+0x88] = ebp** (выровненный размер) | 0x1C34B | |
| [RSI+0xA8] = ebx (frame alignment), [RSI+0x68] = расчёт периода | 0x1C340, 0x1C354 | |
| R15 = RSI − 8 (потом используется как `rcx` в 0x9D74) | 0x1C37C | |
| Если stream+0x84 != 0: [rsi+0x98] = [r14+0xCC0], [rsi+0x9C] = /…, [rsi+0xA0] = [r14+0xCC8] | 0x1C382–0x1C3AE | быстрый путь через r14=[rsi+0xa28] |
| Иначе: вызов 0x9D74(rcx=R15, rdx=[rsi+0xa28]+0x21, r8=[r14]+0x38 call→rax+0xC000, [rsp+0x20]=&off, [rsp+0x28]=&size) | 0x1C3E8–0x1C414 | |
| [RSI+0x98] = size (RBX), [RSI+0x9C] = size/result_of_0x9BB4 | 0x1C42C, 0x1C446 | |
| Третий косвенный вызов vtable+0x38 → RAX += [rsp+0x40] → **[RSI+0xA0] = адрес выходного буфера** | 0x1C44F–0x1C45E | RAX = bar1_base + buffer_offset из BufferStruct |
| R12 → **[R13] (выходной параметр)**; [rsp+0xa0]=ebp size; [rsp+0xa8]=0; [rsp+0xb0]=2 | 0x1C465–0x1C486 | |

**Вывод (доказано кодом)**: `[RSI+0x90]` — результат второго косвенного вызова (kernel-mapping API, адрес виртуальной памяти буфера), `[RSI+0xA0]` — из третьего вызова (vtable+0x38), складывающего BAR1 base + offset из BufferStruct (аналогично 0xB470 memset path). Это **два разных источника**; считать их одним буфером нельзя без дальнейшей трассировки (см. §6).

### 3.4 Static: writer-функция 0xA44C–0xB462 — найден путь записи PCM
Проверено дизассемблированием 2026-10-05 (`py disasm2.py dis A44C AC00`, `dis B300 B463`). **Это закрывает главный ранее «UNKNOWN» (PCM write path из BUFFER_PATH_ANALYSIS.md:523–537) на уровне статического анализа:**

- 0xA44E: `mov r14d, edx` — **RDX = число байт от источника [RSP+0x48]** (источник данных).
- 0xA48E–0xA4C7: если флаг 0x12000==0 (offline/прямой режим): цикл `0x1A9C(dest=rsi+0x168, src=rsi+0x98+offset, len)` — копирование через внутренний буфер stream+0x168.
- 0xA4E2–0xA503: проверка флага 0x14680 и 0x42C0(global 0x14690) — активен ли BufferStruct-режим.
- 0xA510–0xA541: `[rsi+0xa30]` (дескриптор устройства в mapped BufferStruct): проверка `[dev+0xE954] != 0`, format fields `[dev+0x71D0]/[dev+0x71D4]` (bytes_per_frame/…), требование частного == 4 (0xA53F–0xA541).
- 0xA547–0xA57F: формат из `[rsi+0x158]`: `word [+0xE]` (container size, 0x10 → делитель 2, иначе 4 — 0xA558/0xA564/0xA56D), `word [+2]` — вычисление **frames = bytes / divisor** и bpf.
- 0xA5B9–0xA5D6: `0x4140(global 0x14690, dev, count)` → **RAX = destination pointer, [RSP+0x48]** (источник-старший для copy-back? нет — это dest для 0xB3E9: см. ниже).
- 0xA5DC–0xA5F0: `[rsi+0x80] → [+0x8B00]` — конфиг смешивания; `[rsp+0x38]`.
- **Ветка 6 каналов**: 0xA5FE `cmp edi,2` (divisor 2 → 24-bit?) … 0xA600 `cmp ebx,6; jne 0xa8a3` — при 6-канальном формате идёт обработка по каналам (0xA607–0xA89E): цикл по 6 каналам (ebx 0..5, проверка битов канала по байтам [0x1468C] @0xA64D–0xA6B4), DSP-подпрограммы (0x80F4, 0x94CC, 0x7B78, 0x6EB8, 0x7EA4, 0x8BBC, 0xBA64, 0xBBAC) и 4-полосный biquad-фильтр с константами (0xAA44–0xAAF4, mulsd на 0x3FF9D24DE87F66D0 и др.). **Здесь происходят обработка каналов и динамическая обработка (volume/DSP)**.
- **Цикл записи в mapped-буфер**: 0xB398–0xB40D:
  - 0xB3A7: `call 0x9A24(rsi, offset, size)` — вычисление позиции чтения.
  - 0xB3AC–0xB3B8: `lea r8d,[r14+rax]`; **`cmp r8d, dword [rsi+0xA0]`** — проверка границы по размеру буфера (+0xA0 stream).
  - 0xB3C4/0xB3FE: **`add rcx, qword [rsi+0xA8]`** — RCX = база? + frame alignment; обёртка через 0x9A24.
  - 0xB3E1/0xB405: **`mov rdx, qword [rsp+0x48]`** — источник данных.
  - 0xB3E9/0xB40D: **`call 0xCE80`** — memmove с кольцевой обработкой.
  - 0xB412–0xB415: KfRaiseIrql/KfLowerIrql парно — запись под повышенным IRQL.
- **0xCE80 — memmove** (подтверждено кодом, `dis CE80 CF80`): `sub rdx, rcx; jb fail`, копирование qword/byte (<0x4F), выровненный SSE путь для больших размеров. Это **копирование, не zero-copy со стороны T2** (см. §5).
- 0xB438–0xB43E: завершение, epilogue 0xB457–0xB462.

**Источник [RSP+0x48]** = результат `0x4140(global 0x14690, dev_descriptor, count)` @0xA5C0–0xA5CB — резервируемая область в mapped BufferStruct/BAR (ring-буфер T2 side). Точная семантика 0x4140 — гипотеза (§6).

### 3.5 Static: функция 0xC550–0xC8DC — конвертер формата (→ Windows stereo endpoint)
Проверено дизассемблированием 2026-10-05 (`dis C550 C8DC`):

- 0xC566: проверка флага `[0x12004]`; 0xC573–0xC5AE: прямой режим — копирование `rsi+0x98 → rdi+0x250` через 0x1C3C.
- Иначе (BufferStruct-режим, 0xC5CB–0xC5F1): проверки 0x14680, [rcx+0xA0]≠0, 0x42C0(global).
- 0xC5F7–0xC60F: `[rdi+0xa30]` → `[dev+0xE954]`; если < 3 → `[rdi+0x2A8] = значение` и выход. Т.е. 0xC550 обрабатывает **capture/другой режим** при малых значениях.
- 0xC614–0xC6E0: выборка формата: `[rsi+0x158]: word +2` → EBX (sample bytes), `word +0xE` → AX, `cmp ax, 0x10` → выбор 2 или 4 байт (0xC66E/0xC68E cmovne r13d, ecx=4); вычисление frames = esi / (r13 * ebx); вызов 0x9BD0 — количество доступных байт.
- **Цикл преобразования** (0xC6EE–0xC8BE): для каждого фрейма:
  - 16-бит ветка (0xC714–0xC753): чтение `word [rcx + rax]`, где **rax = qword [rdi+0xA8]** (0xC72A) + индекс; запись `word [r12 + r13]` в локальный буфер rdi+0x250; позиция: `lea eax,[r14+2]; div dword [rdi+0xA0]` — **модуль по размеру буфера [rdi+0xA0]** (0xC743).
  - 24/32-бит ветка (0xC759–0xC7FE): `r8 = r14 + qword [rdi+0xA8]` (0xC76E); чтение dword [r8]; при `[0x8B00]→dword==4` и спец-условии — деление на 256 (sar 6, sar 8: 0xC7AE–0xC7DD) — **преобразование 24-in-32 → 16-bit**; при этом на 0xC7B5 вызов `0xCCBD(rdi+0xa30+0x21, "Codec N" @0xD700)` — лог; запись `word [r12+r13]`.
  - 32-бит ветка (0xC846–0xC87B): чтение `dword [rcx + rax]`, rax=qword [rdi+0xA8] (0xC853), `div dword [rdi+0xA0]` (0xC86A).
- Аналогично 0xB3xx: здесь **[RDI+0xA8] — смещение чтения (read cursor), [RDI+0xA0] — размер кольцевого буфера**. RDI — stream object того же типа, что RSI в 0xA44C (см. §6, гипотеза).

**Вывод**: 0xC550 — преобразование формата 24-in-32 → 16-bit для **ввода** (capture/bridge), дающее Windows stereo 16-bit поток. Связь с Windows stereo playback endpoint — отдельный вопрос (§6).

### 3.6 Исправления противоречий в рабочих заметках
По заданию подтверждено дизассемблированием 2026-10-05:
1. **0x9D74: чтения output address/size на RVA 0x9E78 и 0x9E83** (не 0x9DC8/0x9DD0, как в AppleAudio_Playback_Path_Report.md:59–60). На 0x9DC8/0x9DD0 находится `imul rsi, rax, 0x110` + `call 0xCCB7` (input path, версия 1). Отчёт Playback_Report.md требует правки.
2. **0x98 = BS header 0x10 + input_streams 0x84 + buffers 0x04** — подтверждено (0x5F0C = 0x10+0x5EF8+0x04 для output; 0x98 = 0x84+0x04+0x10 выравнивание для input по Function_46F8_Analysis.md:130–136). Никаких «дополнительных Windows layout» полей перед buffers[] из этого не следует — гипотеза о `stream_windows` extension (AppleAudio_BufferRegistration_Report.md:152–219) помечается как **не подтверждена**, layout stream совпадает с Linux (+0x04 buffers) после пересчёта баз.
3. **Шесть независимых каналов — один interleaved-поток**: подтверждено структурой — device 0x39 имеет единственный stream 0x3A (NOTES.md:58), а 0x1C240 выделяет один буфер на stream. Отдельные streams не требуются. Прежние формулировки «1 interleaved ИЛИ 6 раздельных?» (Playback_Report.md:158–168) разрешены в пользу interleaved.
4. Native Speaker format уже получен (48kHz/6ch/24-in-32 interleaved) — NOTES.md:57–73; Playback_Report.md:170–171 («следующий пробел — формат PCM») устарело. Новый вопрос: связь с Windows stereo endpoint.

---

## 4. Доказанная цепочка (data path, playback)

```
Windows KS pin (stereo PCM16 → 24-in-32 buffer, stream+0x168 stage)   [0xA44C: RDX bytes, source 0xA4C0→0x1A9C]
   ↓ (BufferStruct mode: flags 0x14680, 0x42C0, dev+0xE954)
Формат/каналы: [stream+0x158] word+0xE/+2 → divisor 2|4; 6ch → per-channel DSP
   [0xA558–0xA57F; 0xA607–0xAAF4: biquads, 0x80F4/0x94CC/…]
   ↓
0x4140(global 0x14690, dev, count) → destination staging [RSP+0x48]   [0xA5C0–0xA5CB]
   ↓
KfRaiseIrql → 0x9A24(stream, offset, size) → позиция                   [0xB390–0xB3A7]
   ↓
Граница: cmp r8d, [stream+0xA0]; wrap: mod [stream+0xA0]               [0xB3B8, 0xB3D0]
   ↓
add rcx, [stream+0xA8]  (база+offset чтения)                           [0xB3C4, 0xB3FE]
   ↓
0xCE80 = memmove(src=[RSP+0x48], dst=ring, size) — КОПИРОВАНИЕ          [0xB3E9, 0xB40D; 0xCE80 confirmed]
   ↓ (KfLowerIrql 0xB412)
T2 ring buffer (BAR1 + BufferStruct offset) ← читается T2 DMA
```

Destination-side выделение: AllocateOutputBuffer 0x1C240 → `[stream+0xA0]` = bar1_base + offset из BufferStruct (через vtable+0x38 call @0x1C44F), `[stream+0x90]` = из vtable+0x28 call @0x1C324 (второй источник, связь не доказана), handle → R12 → [out].

**Zero-copy: НЕ доказано.** 0xCE80 — подтверждённый memmove; данные копируются в кольцевой буфер под повышенным IRQL. Прежнее утверждение Playback_Report.md:129 («драйвер НЕ копирует PCM, zero-copy») — **отзывается**.

---

## 5. Гипотезы (НЕ подтверждено)

1. **RSI в 0xA44C / RDI в 0xC550 = тот же stream object, что RSI в 0x1C240** (тогда +0xA8/0x9A24-база 0xB3C4 соответствует +0xA0 в Allocate). Основание: одинаковые смещения 0xA0/0xA8/0x98, типовые проверки `[stream+0x84]`, `[rsi+0xa30]` дескриптор BufferStruct-устройства. **Не доказано происхождением указателей** — требуется трассировка vtable-диспетчеризации (кто вызывает 0xA44C/0xC550/0x1C240).
2. **Источник [RSP+0x48] в 0xA44C** = результат 0x4140 — staging-область (вероятно, в mapped BAR1/BufferStruct). Внутренности 0x4140 не разобраны.
3. **0xC550 = capture/конвертер** 24-in-32→16-bit — назначение поля `[dev+0xE954]` (<3 → выход) неизвестно.
4. **Как Windows stereo endpoint связан с 6ch native**: вероятная цепочка — stereo PCM16 upmix → 24-in-32 6ch (0xA44C DSP) → T2 ring. Обратное (capture) — 0xC550. Upmix-код до 0xA44C не трассирован (KS pin → 0xA44C caller).
5. **+0x90 и +0xA0 в 0x1C240 — один буфер или два**: не доказано ни то, ни другое.

## 6. Не удалось восстановить / оставить как UNKNOWN

- **Полный лог сессии 2026-10-05 (фаза 4)**: вывод дизассемблирования (0x1C240, 0x9D74, 0xCE80, 0xA44C, 0xC550) не был сохранён в файлы; в этом логе он восстановлен повторным выполнением тех же команд `py disasm2.py dis …` — результаты совпадают с приведёнными здесь RVA.
- **Git-история** — репозитория нет; какие файлы менялись/создавались, известно только по меткам LastWriteTime (перечислены в §2).
- **Промежуточные черновики/отчёты, если были** — следов нет.
- **Кто вызывает 0xA44C / 0xC550 / 0x1C240** — xref-трассировка vtable не выполнена в этой сессии (команда `py disasm2.py xref <addr>` доступна).
- **0x4140, 0x9A24, 0x9BB4, 0x42C0, 0x80F4/0x94CC и др. DSP-функции** — не дизассемблированы.

## 7. Рекомендованные следующие шаги (статика, безопасно)

1. `py disasm2.py xref A44C` / `xref C550` / `xref 1C240` — найти vtable-слоты и доказать, что RSI/RDI — один тип stream object (закроет гипотезу §5.1).
2. Разобрать `0x4140` (источник [RSP+0x48]) и `0x9A24` (позиция чтения/записи).
3. Сравнить объект-владельца полей +0x88/+0x90/+0xA0/+0xA8 между 0x1C240, 0xA44C, 0xC550.
4. Записать дизассемблирование фазы 4 в отдельный файл (например, `disasm_phase4.txt`), чтобы не потерять.

**Сохранённые ограничения**: только статический анализ; не устанавливать драйвер; не менять MMIO; не запускать команды управления потоком (START_IO/STOP_IO, StreamControlTest не выполняется).

---

## 8. Фаза 5 — восстановление stereo -> 6ch (начало)

### Этап 1: pool buffer и прямой caller (2026-10-05)

Команды и сохранённые результаты: `disasm_phase5.txt`.

#### Подтверждено

- `0x4140` получает `RDX` как объект устройства/потока и `R8D` как requested count.
- `0x4157`: `ESI = [RDX+0x71D0]`.
- `0x417F`: `ExFreePoolWithTag` через IAT `0xE168`.
- `0x4185`: вычисление размера `[device+0x71D0] * requested_count`.
- `0x4195`: `ExAllocatePoolWithTag` через IAT `0xE0B8`.
- `0x419B`: результат сохраняется в `[device+0xE968]`.
- `0x41AF`: capacity сохраняется в `[device+0xE960]`.
- `0x41C6`: обнуление pool buffer через `0xCD80`.
- `0x41CB-0x41D7`: возврат `[device+0xE968]`.

Следствие: источник `[RSP+0x48]` в `0xA44C` после `0xA5C0-0xA5CB` — pool/staging buffer от `0x4140`, а не mapped BAR/T2 memory.

#### Происхождение `this` в прямом пути

- `0xBE36`: `mov edx, edi`; `0xBE39`: `call 0xC550`.
- `0xBEF3`: `mov rcx, rbx`; `0xBEF6`: `call 0xA44C`.
- Это доказывает, что `0xA44C` получает `RCX=RBX` caller-object, и что `0xC550` вызывается из той же функции с тем же сохранённым объектом `RBX`.
- `py disasm2.py xref 1C240` вернул `no xrefs to 0x1C240`; это virtual/interface method и прямой call-site не найден.

#### Пока не доказано

- Тождество `RBX` в `0xBEF6` и `RSI` в `0x1C240`.
- Соответствие полей `+0x90/+0x98/+0xA0/+0xA8` между двумя контекстами.
- Конкретный DSP-алгоритм вызываемых подфункций.

Следующий этап: сохранить полный disassembly ветки `0xA607-0xA8A3`, классифицировать шесть итераций и проследить вызовы/объектные vtable-цепочки.

### Этап 2: ветка 6 каналов и перемещение данных (2026-10-05)

Команды и полный сохранённый вывод: `disasm_phase5.txt`, Stage 2.

#### Подтверждённое разделение формата

- `0xA55D`: `EDI = WORD [format+2]` — число входных каналов.
- `0xA558/0xA564`: `ECX` выбирается как `2` или `4` — байтный divisor/sample container.
- `0xA5F5`: проверяется `EDI == 2`.
- `0xA5FE`: `EBX == 6` выбирает 6-канальную ветку.

`EDI` не является размером сэмпла; `EBX` не является input-channel count.

#### Таблица 6-канальной ветки

Входной адрес вычисляется в `0xA630`: `RDI = [RSI+0x98] + input_offset`. Каналы `EBX=0..5` идут циклом. В `0xA63F-0xA646` адрес записи вычисляется как `R15 = pool_base + 4*EBX`; запись значения происходит в `0xA84F` (`DWORD [R15]`).

- Канал 0: `WORD [RDI]` (`0xA7E6`), disable bit 0 (`0xA64D-0xA655`), state `[RSI+0x2B0]/[RSI+0x350]`, вызов `0x94CC` (`0xA813`), output `pool+0`.
- Канал 1: `WORD [RDI]` (`0xA7D5`/`0xA7D8`), disable bit 1 (`0xA660-0xA668`), видимый путь clamp/scale без названного вызова, output `pool+4`.
- Канал 2: `WORD [RDI]` (`0xA799`), disable bit 2 (`0xA673-0xA67B`), state `[RSI+0x530]/[RSI+0x5D0]`, вызовы, выбираемые через `0xA744-0xA747` (`0x80F4`, `0x6EB8`, `0x7EA4`, `0x8BBC`, `0xBA64`, `0xBBAC`), output `pool+8`.
- Канал 3: `WORD [RDI+2]` (`0xA768`), disable bit 3 (`0xA686-0xA68E`), state `[RSI+0x670]/[RSI+0x710]`, тот же selector-dependent family, output `pool+12`.
- Канал 4: `WORD [RDI+2]` (`0xA715`), disable bit 4 (`0xA699-0xA6A1`), state `[RSI+0x8F0]/[RSI+0x990]`, `0x80F4` (`0xA747`), output `pool+16`.
- Канал 5: `WORD [RDI+2]` (`0xA6F2-0xA710`), disable bit 5 (`0xA6AC-0xA6B4`), видимый путь clamp/scale без названного вызова, output `pool+20`.

Для всех каналов set-bit ведёт к `0xA82D`, затем ноль записывается через `0xA84F`. После вычисления значение масштабируется `shl r8d, CL` (`0xA830-0xA84C`), где `CL = [device+0x71D8] - [format+0xE]`.

Это доказывает два исходных word-позиционных класса (`[RDI]` и `[RDI+2]`), per-channel state и disable mask. Это **не доказывает**, что эти слова являются stereo PCM L/R samples; физическая привязка каналов к динамикам также не доказана.

#### Цепочка source -> pool -> ring

- Прямой/offline source path: `0xA4B6` добавляет `[RSI+0x98]` к offset, `0xA4C0` задаёт destination `[RSI+0x168]`, `0xA4C7` вызывает `0x1A9C`.
- BufferStruct path: `0xA5B9` загружает global `0x14690`, `0xA5C3` загружает device `[RSI+0xA30]`, `0xA5C6` вызывает `0x4140`, `0xA5CB` сохраняет возвращённый pool pointer в `[RSP+0x48]`.
- Ring destination/position: `0xB3A7` вызывает `0x9A24`; `0xB3B8` и `0xB3D0` проверяют/ограничивают по `[RSI+0xA0]`; `0xB3C4/0xB3FE` добавляют `[RSI+0xA8]` к destination base.
- Copy: `0xB3E1/0xB405` передают `[RSP+0x48]` как source; `0xB3E9/0xB40D` вызывают `0xCE80`. Код `0xCE80` выполняет qword/byte/SSE copy.

Следствие: подтверждён `pool staging -> ring copy`; zero-copy утверждать нельзя.

#### Исправление старых формулировок

- `+0x90` больше не называется `active_flag`: в `0x1C240` это результат второго косвенного вызова (`0x1C324 -> 0x1C332`), а в writer-контексте это числовое поле, используемое как divisor/размерный параметр (`0xA48E`, `0xB3B8`); семантика зависит от подтверждения объекта.
- Не используется утверждение о дополнительном Windows layout перед `buffers[]`: одинаковые смещения сами по себе не доказывают layout.
- `0x4140` не называется mapped BAR/T2 buffer: это kernel pool allocation (`ExAllocatePoolWithTag`/`ExFreePoolWithTag`).
- Не утверждается соответствие EBX 0..5 физическим динамикам.

Остаётся следующий шаг: доказать или опровергнуть тождество `RSI`/`RBX` через конструктор и vtable/interface dispatch; прямой `xref 0x1C240` ничего не нашёл, что согласуется с виртуальным вызовом.

### Этап 3: интерфейсный `+8` и границы доказательства (2026-10-05)

Команды и результаты сохранены в `disasm_phase5.txt`, Stage 3.

#### Доказано

- В `0x19E28` вызывается `0x1C570` с `RCX=RBX`, где `RBX` — создаваемый/инициализируемый внутренний объект.
- `0x19E3D-0x19E4D` возвращает наружу `RBX+8` (`lea rdx,[rbx+8]`, conditional select, `mov [r12],rcx`).
 - **SUPERSEDED:** прямое чтение функции/указателя в RVA `0xF100` само по себе не доказывает начало vtable или первый слот; объектная база и границы таблицы не установлены.
- `0x1C266` сохраняет входной interface pointer в `RSI`; `0x1C37C` вычисляет `R15=RSI-8`; `0x1C411-0x1C414` передаёт R15 в `0x9D74`.

Это доказывает +8 adjustment для объекта/интерфейса `0x1C240`.

#### Не доказано

- `RBX` в прямом caller `0xBEF6 -> 0xA44C` статически не связан с allocation `RBX` из `0x19DE5`.
- Поэтому кандидаты соответствий `A44C+0x90 -> Allocate+0x88`, `A44C+0x98 -> Allocate+0xA0`, `A44C+0xA0 -> Allocate+0xA8` пока только согласованная гипотеза по полям и +8, но не доказательство одного объекта.
- `py disasm2.py xref 1C240` дал `no xrefs`, что согласуется с виртуальным вызовом, но не восстанавливает вызывающий объект.

#### Исправление старых утверждений

- Старое объявление `+0x90 = active_flag` исключено из новых выводов. В Allocate-контексте это результат косвенного вызова `0x1C324 -> [RSI+0x90]`; в writer-контексте `[RSI+0x90]` участвует в делении/размерных расчётах (`0xA48E`, `0xB3B8`). Называть его active flag нельзя.
- Старое утверждение о дополнительном Windows layout перед `buffers[]` не используется: одинаковые смещения не являются доказательством layout.

#### Итог фазы 5 на данный момент

Доказано: `0x4140` даёт pool staging pointer; шесть каналов обрабатываются отдельным циклом; `0xA44C` передаёт pool buffer в `0xCE80`, который копирует данные в кольцевой destination; interface `+8` для `0x1C240` доказан.

Не доказано: точная семантика двух word-источников как stereo PCM L/R, тождество объекта `0xA44C` и `0x1C240`, и соответствие индексов 0..5 физическим динамикам.

## 9. Фаза 6 — KaiT2en portability inventory and Windows plan (2026-10-05)

### Источник KaiT2en

- Локальный checkout найден в `kait2en/`.
- `.git` отсутствует: commit SHA и историю checkout проверить невозможно; SHA не выдумывается.
- Выборочно прочитаны `modules/t2bce_audio/audio.h`, `audio.c`, `pcm.c`, `description.h`, `dsp/README.md`, `dsp/profiles/16_1/graph.json`, `LICENSING.md` и `LICENSE`.

### Результат инвентаризации

- `tools/inventory_kait2en_profile.py` прочитал graph без обработки звука.
- Результат: 108 узлов, 118 links, 2 входа, 6 выходов.
- Узлы: 92 `bq_raw`, 4 mixer, copy/delay/convolver; 8 LV2: autogain (1), bankstown (1), limiter stereo (3), multiband compressor stereo (3).
- Внешние зависимости: три XTC FIR WAV для 44.1/48/96 kHz.
 - Capture-side graph принимает 2 канала FL/FR; playback-side направляет 6 каналов в `alsa_output.hw_Audio_0` как `AUX0..AUX5`. Это topology graph, не доказательство физического порядка динамиков. **SUPERSEDED:** старую таблицу, где каналы 4 и 5 были перепутаны.

### Corrected offset notes

- При `I=O+8` действуют `O+0x90=I+0x88`, `O+0x98=I+0x90`, `O+0xA0=I+0x98`, `O+0xA8=I+0xA0`. Предыдущая запись offset mapping помечена **SUPERSEDED**.
- KaiT2en PCM code описывает negotiated sample format/rate/channels, one remote stream/substream, mapped buffer exposure, ordered workqueue for I/O и timestamp-anchor interpolation. START/STOP code не запускался.

### Решение по переносу

- Рекомендован путь B: Windows audio miniport с явным владельцем T2 transport, mapped/ring resources, stream state и timing; DSP graph/data переносить поверх независимо проверяемого 6ch endpoint.
- Путь A ограничен: текущий KS endpoint подтверждён только как stereo, а native six-channel query является metadata, не PCM write API. Пользовательский DSP перед stereo endpoint не создаёт произвольные 6 независимых выходов.
- AppleAudio internal pointers не рассматриваются как стабильный API. Продолжение reverse engineering оправдано только для закрытия miniport contract/object ownership.

### Артефакты и проверки

- Созданы `CURRENT_STATE.md`, `PORT_PLAN.md`, `tools/inventory_kait2en_profile.py`, `evidence_kait2en_inventory.txt`.
- `py -m py_compile tools\\inventory_kait2en_profile.py` завершён с exit 0.
- JSON `kait2en/dsp/profiles/16_1/graph.json` успешно разобран.
- Hardware/drivers/MMIO/stream-control operations в этой фазе не выполнялись.
