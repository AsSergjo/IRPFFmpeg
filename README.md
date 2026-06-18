# IRPFFmpeg

IRPFFmpeg - настольный проигрыватель интернет-радио для Windows. Он воспроизводит сетевые аудиопотоки через FFmpeg, показывает текущий трек, загружает обложки, ведет историю и умеет записывать эфир в MP3 320 kbit/sec или FLAC.

Проект сделан как легкое Win32-приложение на C++17: без Electron, без браузерной оболочки, с отдельным загрузчиком для аккуратной работы с FFmpeg/SDL2 DLL.

![Главное окно IRPFFmpeg](docs/assets/main-window.png)

## Возможности

- воспроизведение интернет-радио по URL;
- работа с M3U-плейлистом;
- добавление, удаление и переключение станций из интерфейса;
- отображение ICY/stream metadata и технического статуса потока;
- автоматический поиск и кэширование обложек;
- запись текущего эфира в `Rec`;
- экспорт записей в MP3 320 kbit/sec или FLAC;
- добавление метаданных и обложки в записанные файлы, когда данные доступны;
- регулировка громкости, баса и верхних частот;
- Stereo Width, Exciter, DeepBass, Dynamic Auto Volume, GainRider и финальный лимитер;
- сворачивание в системный трей;
- отдельный загрузчик `Start_IRPFFmpeg.exe`, который проверяет runtime DLL и запускает основное приложение.

## Быстрый запуск

Для обычного запуска используйте:

```text
Start_IRPFFmpeg.exe
```

`IRPFFmpeg.exe` лучше не запускать напрямую: основному приложению нужны библиотеки из папки `heap_dll`. Загрузчик проверяет наличие DLL, добавляет папку во временный `PATH` дочернего процесса и запускает плеер.

Минимальный состав релизной папки:

```text
Start_IRPFFmpeg.exe
IRPFFmpeg.exe
heap_dll/
playlist.m3u
```

Во время работы приложение может создать:

```text
app.dat         - настройки и состояние
Rec/            - записанные треки
cover_cache/    - кэш обложек
debug_log.txt   - диагностический лог, если включено логирование
```

## Сборка из исходников

Требования:

- Windows 10/11;
- Visual Studio 2022;
- MSVC toolset v143;
- Windows SDK 10;
- C++17;
- FFmpeg development package с `include`, `lib` и `bin`;
- SDL2 и SDL2_image.

Откройте `IRPFFmpeg.sln` в Visual Studio и соберите конфигурацию `Release|x64`.

В текущем проекте пути к зависимостям прописаны в `IRPFFmpeg.vcxproj`:

```text
D:\Code\ffmpeg-dev\include
D:\Code\ffmpeg-dev\lib
D:\Code\ffmpeg-dev\bin
C:\dev\vcpkg\installed\x64-windows\include\SDL2
C:\dev\vcpkg\installed\x64-windows\lib
```

Если зависимости лежат в другом месте, измените пути в свойствах проекта или в `.vcxproj`.

Подробная инструкция: [docs/BUILD.md](docs/BUILD.md).

## Структура проекта

```text
IRPFFmpeg.sln              - решение Visual Studio
IRPFFmpeg.vcxproj          - основное приложение
Start_IRPFFmpeg.vcxproj    - загрузчик приложения
IRPFFmpeg.cpp/.h           - Win32 UI, управление состоянием, воспроизведение
audio_dsp.cpp/.h           - обработка звука
file_recording.cpp/.h      - запись MP3/FLAC и метаданные
cover_art.cpp/.h           - поиск, загрузка и кэширование обложек
metadata_decode.cpp/.h     - декодирование текстовых метаданных
util.cpp/.h                - общие утилиты
docs/                      - документация для пользователей и разработчиков
```

Подробнее об устройстве: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Документация

- [Руководство пользователя](docs/USER_GUIDE.md)
- [Сборка проекта](docs/BUILD.md)
- [Подготовка релиза](docs/RELEASE.md)
- [Архитектура](docs/ARCHITECTURE.md)
- [Описание для GitHub](docs/GITHUB_REPOSITORY.md)
- [Участие в разработке](CONTRIBUTING.md)
- [История изменений](CHANGELOG.md)
- [Сторонние компоненты](THIRD_PARTY_NOTICES.md)

## Публикация на GitHub

Репозиторий подготовлен как исходный код проекта. Бинарные файлы сборки, папки `x64/`, `.vs/`, временные настройки, кэш обложек и записи исключены через `.gitignore`.

Готовые EXE/DLL лучше публиковать не в git-истории, а через GitHub Releases. Инструкция по составу архива находится в [docs/RELEASE.md](docs/RELEASE.md).

## Лицензия

Код IRPFFmpeg распространяется под лицензией MIT. Подробности: [LICENSE](LICENSE).

Сторонние компоненты распространяются на своих условиях:

- FFmpeg DLL - LGPL/GPL согласно конкретной сборке FFmpeg;
- SDL2 и SDL2_image - zlib license;
- остальные runtime-библиотеки - согласно их собственным лицензиям.

Подробности и релизные требования: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
