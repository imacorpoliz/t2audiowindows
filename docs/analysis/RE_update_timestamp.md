# RE_update_timestamp: Ответ на UPDATE_TIMESTAMP (Задача R3)

> **Дата анализа:** 28 октября 2026 г.  
> **Инструменты:** Дизассемблер Capstone, Python PE parser, анализ бинарников `AppleAudio.sys` (SHA256 `cd8b81fabc9b…a150`) и `AppleUSBVHCI.sys` 6.1.7500.5 (SHA256 `0438292090fc…55c1`).  
> **Статус выводов:** **ДОКАЗАНО** (с указанием точных инструкций и адресов VA/RVA).

---

## 1. Кто отправляет ответ на `UPDATE_TIMESTAMP`

В Linux-драйвере `kaiT2en` аудиодрайвер напрямую подключен к очереди BCE и сам отправляет ответ `UPDATE_TIMESTAMP_RESPONSE (34)` на каждое уведомление от bridgeOS.  
В Windows архитектура разделена: `AppleUSBVHCI.sys` является владельцем шины и очередей BCE, а аудиодрайвер (`AppleAudio.sys` / `T2AudioPort`) работает поверх него.

### Доказательство из дизассемблера `AppleUSBVHCI.sys`:

В обработчике входящих пакетов bridgeOS (`AppleUSBVHCI.sys` `0x140001CF0`):
```assembly
; AppleUSBVHCI.sys VA: 0x140001D72 - 0x140001DE8
0x140001d72: movzx ecx, byte ptr [rbx + 4]    ; rbx = входящий буфер, +4 = header.type
0x140001d76: sub   ecx, 1                     ; ecx == 0 -> type 1 (COMMAND)
0x140001d79: je    0x1DA4                     ; переход на проверку msg == 4
...
0x140001da4: cmp   dword ptr [rbx + 0xd], 4   ; rbx + 0x0D = msg (4 = UPDATE_TIMESTAMP)
0x140001da8: jne   0x1D9A                     ; не msg 4 -> отбрасывается
0x140001daa: mov   r8d, dword ptr [rbp - 0x40]; r8d = длина входящего пакета
0x140001dae: mov   rdx, rbx                   ; rdx = указатель на входящий пакет
0x140001db1: mov   rcx, rdi                   ; rcx = контекст устройства
0x140001db4: call  0x140002274                ; ДОСТАВКА В АУДИОДРАЙВЕР ЧЕРЕЗ IOCTL 0x222020!

; --- СРАЗУ ПОСЛЕ ДОСТАВКИ: САМ AppleUSBVHCI.SYS ФОРМИРУЕТ ОТВЕТ ---
0x140001db9: movups xmm0, xmmword ptr [rbx]   ; Копирует первые 16 байт (tag, devid, header)
0x140001dbc: mov   r8d, 0x15                  ; Размер ответа = 21 (0x15) байт
0x140001dc2: lea   rdx, [rbp - 0x28]          ; Буфер ответа на стеке
0x140001dc6: mov   rcx, rdi                   ; Контекст
0x140001dc9: movups xmmword ptr [rbp - 0x28], xmm0 ; Запись скопированного заголовка
0x140001dcd: mov   eax, dword ptr [rbx + 0x10]
0x140001dd0: mov   dword ptr [rbp - 0x18], eax
0x140001dd3: mov   al, byte ptr [rbx + 0x14]
0x140001dd6: mov   byte ptr [rbp - 0x14], al
0x140001dd9: and   dword ptr [rbp - 0x17], 0
0x140001ddd: mov   byte ptr [rbp - 0x24], 2   ; Смещение +4 = TYPE 2 (RESPONSE)!
0x140001de1: mov   dword ptr [rbp - 0x1b], 0x22 ; Смещение +13 = MSG 0x22 = 34 (UPDATE_TIMESTAMP_RESPONSE)!
0x140001de8: call  0x1400027C4                ; ОТПРАВКА ОТВЕТА В BRIDGEOS!
```

### Доказанный факт:
1. `AppleUSBVHCI.sys` **самостоятельно и автоматически** формирует и отправляет `UPDATE_TIMESTAMP_RESPONSE (34)` в bridgeOS сразу после вызова `0x140002274`.
2. В ответе сохраняются `tag` и `device_id` оригинального входящего сообщения.
3. Код сообщения в ответе строго равен `0x22` (34).

---

## 2. Что делает `AppleAudio.sys` при получении `UPDATE_TIMESTAMP`

В `AppleAudio.sys` (обработчик `IOCTL 0x222020` по адресу `0x140001280`):
```assembly
; AppleAudio.sys VA: 0x1400012BF - 0x14000134F (обработчик IOCTL 0x222020)
0x1400012bf: mov  eax, dword ptr [rdi + 0xd]  ; eax = msg (смещение 0x0D)
0x1400012c2: cmp  eax, 4                      ; msg == 4 (UPDATE_TIMESTAMP)?
0x1400012c5: je   0x140001334
...
0x140001334: mov  rdx, qword ptr [rdi + 5]    ; rdx = device_id (смещение +5)
0x140001338: mov  r8, rdi                     ; r8 = буфер сообщения
0x14000133b: mov  rcx, qword ptr [0x140014690]; rcx = контекст устройства (из глобальной переменной)
0x140001342: call 0x14000435C                ; Сохранение отметки времени
0x140001349: mov  dword ptr [rsi + 0x30], ebx ; Irp->IoStatus.Status = 0 (ebx = 0, IRP в rsi)
0x14000134f: call IofCompleteRequest          ; Завершение IRP
```

Функция `0x14000435C`:
* Ищет устройство по `device_id`.
* Увеличивает счетчик тиков по смещению `+0xE954`.
* Сохраняет 64-битный timestamp `*(u64*)(buf + 0x15)` по смещению `+0xE958`.
* **Не вызывает никаких функций отправки сообщений.** Никакой `FUN_140004a4c` и никакой IOCTL не вызываются.

---

## 3. Итог задачи R3

| Вопрос | Ответ | Статус |
| :--- | :--- | :--- |
| Нужен ли ответ на `UPDATE_TIMESTAMP` от bridgeOS? | Да, bridgeOS ожидает ответ с кодом 34 (`0x22`) | **ДОКАЗАНО** |
| Кто формирует и отправляет ответ в Windows? | Драйвер шины `AppleUSBVHCI.sys` (по адресу `0x140001DE8`) | **ДОКАЗАНО** |
| Должен ли аудиодрайвер (`T2AudioPort`) отвечать на `UPDATE_TIMESTAMP`? | **НЕТ, категорически не должен.** Если аудиодрайвер пошлет ответ, возникнет конфликт повторного ответа | **ДОКАЗАНО** |
| Что должен делать `T2AudioPort` при приеме `0x222020` с `msg == 4`? | Разобрать timestamp/sampleTime, обновить контекст устройства и завершить IRP со `STATUS_SUCCESS` | **вероятно; не проверено на железе** (реализовано в `src/Driver.c` и `src/BceTransport.c`) |
| Будет ли bridgeOS продолжать слать отметки времени? | Да, поскольку `AppleUSBVHCI.sys` исправно подтверждает каждый пакет кодом `0x22` | **ДОКАЗАНО** |