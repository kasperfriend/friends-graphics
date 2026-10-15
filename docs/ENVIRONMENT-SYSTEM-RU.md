# Система окружения: реализация и состояние проверки

Обновление 30.09.2026: пользователь запустил клиент. Новый SHA-256 executable —
`e7c2a69cb86804eb9e21254b7b45c6a03e532d8b8f94451d9e0855f7535f97c6`.
Инструкции getter/setter проверены повторно. Из процесса игры прочитаны Map=1,
Zone=10143, Area=188; база подтверждает Shadowglen и цепочку parent.
Это проверка одной текущей локации, а не всей игровой матрицы.
После закрытия клиента установлены обновлённая DLL и исправленный fingerprint;
сохранены резервные копии и пользовательские настройки GraphicsEffects.
F7 теперь содержит три колонки без страниц и прокрутки. Проверки всех элементов,
mouse hit testing и D3D9 state/reset прошли на 1080p, 1440p и 4K.
Ниже сохранён первоначальный отчёт: упоминания PgUp/PgDn и отсутствия запуска
описывают прежнее состояние первой сборки.

Подготовлены код и Release-сборка. По уточнению пользователя клиент не
запускался и файлы в `C:\games\Ascension` не заменялись. Игровая Definition of
Done ещё не выполнена: требуется проверка локаций в реальном мире и подтверждение
активного источника DBC. Диагностическая сборка не меняет визуальные параметры
по локации до прохождения обоих условий.

1. **Реализовано:** независимый провайдер клиента/локации, база зон, строгий
   конфигурационный парсер, named profiles, приоритет Default → Map → Zone →
   Area, debounce, плавный переход, централизованный снимок множителей в
   `FrameContext`, F7-диагностика, новый шрифт и F12 reload.
2. **Добавлены файлы:** `src/Game/WoWClientContext.*`,
   `src/Game/WoWLocationProvider.*`, `src/Environment/LocationContext.h`,
   `EnvironmentProfile.h`, `EnvironmentDatabase.*`,
   `EnvironmentProfileManager.*`, `OverlayFont.h`, `EnvironmentProfiles.ini`,
   инструменты и README в `tools/EnvironmentDatabaseBuilder`,
   `tools/EnvironmentRegression.cpp`, `tools/OverlayRegression.cpp`,
   `tools/Test-Environment.cmd`, `tools/Test-Overlay.cmd`, этот отчёт.
3. **Изменены файлы:** `.gitignore`, `ModernWoWRenderer.cpp`,
   `ModernWoWRenderer.vcxproj`, `TuningOverlay.h`, `DistanceFog.h`,
   `VolumeIntegration.h`, `WeatherVisuals.h`, `src/Core/FrameContext.h`,
   `src/Effects/DirectionalVolumetricLighting.cpp`,
   `src/Effects/LocalLightingRenderer.cpp`, `tools/AtmosphereRegression.cpp`.
4. **MapID:** основной модуль через `GetModuleHandleW(nullptr)`, затем
   настроенный RVA. Для исследованного executable кандидат — `0x7D088C`.
   Чтение ограничено PE image и readable region, выполняется через
   `ReadProcessMemory` собственного процесса.
5. **ZoneID:** та же схема, кандидат `0x7D080C`; не вызов Lua API и не ID
   области, выбранной на мировой карте.
6. **AreaID:** та же схема, кандидат `0x7D0810`. Два последовательных снимка
   должны совпасть. Известные записи сверяются с map и цепочкой parent; неизвестная
   custom area допускается с известной подходящей zone/map. Сырые кандидаты и
   причина отказа доступны в F7.
7. **Именно Ascension:** исследован установленный `Ascension.exe`, SHA-256
   `f4b9f6fce448194638c5b1c751483090a48c597c6272237f31b3d151b50d3114`,
   timestamp `0x4C2452FE`, image size `0x9FD000`, preferred base `0x400000`.
   Поддержка в игровом мире пока не подтверждена. Другой hash не включает
   чтение reference offsets автоматически.
   Отпечатки обычного (stock) `Client\wow.exe` 3.3.5a сборки 12340 записаны здесь, чтобы
   клиент игрока можно было опознать, а не угадывать: MD5
   `54fa1a3520cf07422e560994b00b8029`, SHA-256
   `57dd8955fd7238b00969f6011cdaa13dca14daa5849d1f9be64152bd4c7fe5da`. Это не тот executable,
   который заверен в поставке, поэтому автоматического доверия к нему нет: RVA для него
   прописываются вручную в `[LocationProvider]` (шаблон с этими значениями и с кандидатами
   `0x7D088C` / `0x7D080C` / `0x7D0810` лежит комментарием рядом с секцией), а
   `VerifiedInWorld` остаётся 0, пока F7 не покажет корректные id в пути. Hex и десятиличные
   значения читаются одинаково.
8. **Проверка адресов:** offline PE/disassembly. Getter в RVA `0x119640`
   читает candidate MapID; lookup в `0x119CA0` читает candidate AreaID и
   обращается к таблице; setter в `0x1204E4` записывает candidate ZoneID/AreaID
   вместе. Найдены регистрации GetZoneText/GetSubZoneText, которые возвращают
   строки, а GetCurrentMapAreaID работает с выбранной картой UI — он не
   использован как location source. Pattern scan не добавлен: нет проверенной
   переносимой сигнатуры. Имеется независимая конфигурация build-specific RVAs.
9. **DBC extraction:** MPQ hash lookup через mpyq, strict WDBC parsing,
   проверка размеров, string offsets, map/parent references и циклов.
   Неопределённый приоритет архивов останавливает авторитетный экспорт.
   Есть режимы verified archive manifest, client VFS export и диагностический
   экспорт одной версии.
10. **AreaTable:** фактический `Data\patch-M.MPQ` содержит 2849 записей AreaTable
    и 374 Map; это исследованный custom-кандидат, а не подтверждённый победитель
    loader priority. В locale-архивах обнаружены отличающиеся версии.
    Диагностическая база и metadata находятся в `build/client-research`.
    Автоматически сгенерированы demo IDs 12 (Elwynn), 10 (Duskwood), 440
    (Tanaris), 618 (Winterspring) — именно из этих клиентских данных.
11. **Inheritance:** каждый слой меняет только заданные множители; named
    profile может наследовать другой named profile. Отсутствующие поля
    сохраняют нижний слой. Ссылку `Profile=Default` трактуем как отсутствие
    дополнительных named overrides, без сброса уже применённого Map/Zone.
    Ошибка имени/цикл/дубликат/NaN/неизвестный параметр отвергает весь reload и
    сохраняет предыдущую конфигурацию. Множители ограничены диапазоном 0–4.
12. **Эффекты:** density основной атмосферы, local fog density, atmospheric
    scattering, sun/moon screen shafts, surface local lighting и volumetric
    local-light scattering, native/continuity precipitation strength,
    distance/edge fog power. Базовые настройки и переключатели эффектов не
    изменяются; native weather type, положение солнца/луны, вода и shadows
    сохраняют существующую логику. Остальные параметры пока не переопределяются.
13. **Transition:** configurable smoothstep interpolation всех семи float
    множителей, по умолчанию 4 секунды. Новый переход начинается от текущего
    видимого состояния. Поллинг — 8 раз/с, не draw-call. Area debounce 300 ms
    и минимум три совпадающих снимка; смена map требует минимум три снимка.
    Transient invalid readings не переключают профиль сразу, устойчивый отказ
    плавно возвращает к Default. При закрытой verification gate множители
    остаются единичными, включая Default.
14. **F7:** location IDs/names, сырые кандидаты при отказе, validation status,
    источник профиля, transition, состояние проверки. Настройки остаются base
    user settings. Доступен experimental API `CreateOverride`; запись кнопками
    в UI выключена. Меню имеет страницы PgUp/PgDn, чтобы все настройки
    оставались доступными после увеличения текста.
    Причину отказа теперь несёт первая строка F7 ("Location unavailable: not configured",
    "unrecognized executable", "ids fail area database" или "no world frame yet"): шапка рисует
    ровно четыре строки, и объяснение на пятой было бы невидимым. Провайдер настраивается до
    разбора EnvironmentProfiles.ini, поэтому отсутствующий или отвергнутый файл профилей больше
    не выключает определение локации; лог получает строку "Location unavailable: <причина>" с
    подсказкой, какой файл положить рядом с d3d9.dll. Значения [LocationProvider] обрезаются с
    краёв: хеш или RVA с пробелом рядом больше не читаются как "unrecognized executable", хотя
    хвостовой мусор по-прежнему остаётся ошибкой. Пока ни одна локация не подтверждена, hold не
    блокирует опрос — world frame по-прежнему обязателен для самого сэмплирования, но «кадра с
    глубиной ещё не было» больше не может означать «локации никогда не будет».
15. **Шрифт:** системный Segoe UI normal. Он читаемее прежнего bitmap 5×7 и
    поддерживает кириллицу. Font binary в repository не добавлен. GDI строит
    atlas только при создании/смене размера, D3D9 рисует textured glyph quads.
16. **DPI/fallback:** 16 px при 1080p, 21 px при 1440p, 32 px при 4K до
    ограничений доступной ширины; учтён `GetDpiForWindow`, высота viewport и
    размер клиентского окна. Системная подстановка face допустима; при ошибке
    atlas используется прежний bitmap font. Ошибка не вызывает повторную
    генерацию каждый кадр. Device reset освобождает atlas; новый создаётся при
    следующем открытом overlay. Реальный Windows high-DPI input mapping ещё
    требует проверки в игре.
17. **F12:** существующие readers продолжают reload GraphicsEffects; добавлен
    transactional reload EnvironmentProfiles, повторная конфигурация provider
    и синхронизация видимых F7 base values. База не перечитывается каждый F12.
    Файл профилей лежит рядом с DLL. Не происходит записи GraphicsEffects при
    переходе локации.
18. **Проверки:** Release Win32 build, EnvironmentRegression (priority,
    unknown fallback, invalid configs, interpolation, debounce, database
    hierarchy/cycles, invalid RVAs и unknown executable), пять Python DBC
    tests, существующий AtmosphereRegression на реальном D3D9 (атмосфера,
    local fog, native rain persistence, water/reflections, state/reset),
    дополнительная GPU-проверка zero environment density без изменения base,
    OverlayRegression на 1920×1080 / 2560×1440 / 3840×2160 с сохранёнными
    preview PNG, проверкой кириллицы/цифр, state restoration и font recreation.
    Эти проверки не заменяют тесты реального клиента.
19. **Ограничения:** нет in-world validation, verified archive priority,
    portable signature scan, достоверного native world-loaded sentinel,
    indoor/outdoor detection, художественного утверждения demo profiles и
    измеренного client FPS. World context пока оценивается по camera/depth
    render context, что может быть недоступно в некоторых режимах.
    High-DPI mouse mapping, Alt+Tab и игровые переходы ещё не подтверждены.
    Не заявляется zero regressions для всех игровых сцен.
20. **Следующий этап:** получить client VFS DBC export или подтверждённый
    список активных MPQ, зайти на персонажа, сравнить F7 IDs/names в Elwynn,
    Duskwood, другой карте/instance, проверить teleport/loading/login, затем
    отметить RVAs VerifiedInWorld и использовать базу с подтверждённой
    provenance. Только после этого включать автоматические художественные
    overrides и проходить игровую матрицу из задания.

Сборка:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe' ModernWoWRenderer.sln /p:Configuration=Release /p:Platform=Win32 /m
cmd /c tools\Test-Environment.cmd
cmd /c tools\Test-Overlay.cmd
cmd /c tools\Test-Atmosphere.cmd
python tools/EnvironmentDatabaseBuilder/test_builder.py
```

Подготовленный пакет `build/Ascension-environment-diagnostic` содержит DLL,
EnvironmentProfiles с exact-hash кандидатами и `VerifiedInWorld=0`, demo sections
из patch-M и diagnostic-only database. Он не установлен в игровой клиент.
При будущем ручном тестировании сохранить действующие GraphicsEffects.ini и
ModernWoWRenderer.ini, сделать резервную копию DLL, затем установить только
файлы диагностического пакета рядом с executable. Оба validation gate нельзя
менять лишь ради получения эффекта: они означают выполненные проверки.

### Подтверждение загруженных таблиц 2026-09-30
Read-only verify_live_database.py сравнил таблицы работающего Ascension (PID 11624): 2849 AreaTable и 374 Map. Все ID, связи и флаги совпадают с patch-M; Map совпадает полностью. Единственное отличие названия: Area 1 — пустая строка в архиве, Dun Morogh в памяти клиента. Экспорт areas-live.txt содержит фактические строки из памяти; provenance=verified-live-client-tables. Отчёт live-database-verification.json сохраняет хеши и результат сравнения. DLL и база установлены в клиент с резервной копией, общие настройки GraphicsEffects.ini сохранены. Требуется перезапуск клиента; автоматические профили активируются после него. Отдельное автоматическое сохранение ползунков F7 по каждой локации ещё не реализовано; изменения ползунков относятся к общей базе, локальные overrides задаются в EnvironmentProfiles.ini.


### Локальные настройки F7
Настройки эффектов F7 сохраняются в LocationGraphics.ini по Map.Zone.Area. Исключение — весь блок IMAGE & COLOR (GLOBAL): включение PostProcess, яркость, контраст, гамма и резкость сохраняются только в общем GraphicsEffects.ini; старые локальные значения PostProcess игнорируются. При Area=0 сохраняется зона. Чтение наследуется Map -> Zone -> Area, затем отсутствующие значения берутся из GraphicsEffects.ini. Локальные настройки остальных эффектов не изменяют общий файл, пока локация подтверждена. Вне валидной локации или без подтверждённых адресов/базы (например, на обычном клиенте 3.3.5) все ручки F7 редактируют общие разделы GraphicsEffects.ini: зона/подарея не предлагается, а ранее «замороженное» меню больше не показывает ручки, которые нельзя изменить. При смене локации перечитываются настройки всех эффектов — но не чаще одного раза в секунду и только после того, как локация устоялась: на границе зон она на секунду «дрожит», и раньше каждая такая дрожь вызывала полное перечитывание. Проверено: отдельный инст и открытый мир, возврат и восстановление, Area 0, нулевые значения, отсутствие записи при загрузке и unverified. Переключатели/качество меняются сразу; плавные множители EnvironmentProfiles.ini работают отдельно. Для сброса локальных настроек удалить соответствующие секции LocationGraphics.ini и нажать F12 (или перезапустить).
