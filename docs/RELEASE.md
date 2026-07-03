# Подготовка релиза

Готовые бинарные сборки лучше публиковать через GitHub Releases, а не хранить в репозитории.

## Перед сборкой

1. Соберите `Release|x64`.
2. Запустите `Start_IRPFFmpeg.exe` из `x64\Release`.
3. Проверьте воспроизведение хотя бы одной станции.
4. Проверьте старт без Visual Studio.
5. Проверьте запись короткого фрагмента в MP3.
6. Проверьте запись короткого фрагмента в FLAC, если менялась логика записи.
7. Проверьте, что приложение стартует после удаления `app.dat`.

## Схема assets

Релизы разделяют сторонние DLL и файлы проекта.

`IRPFFmpeg-support-win-x64.zip` - стабильный DLL-support архив. Он содержит только сторонние runtime DLL в папке `heap_dll` и не привязан к версии приложения. Публикуйте его заново только тогда, когда меняется набор DLL.

`IRPFFmpeg-vX.Y.Z-project-win-x64.zip` - версионный project-архив. Он содержит только файлы проекта IRPFFmpeg и не содержит сторонних DLL.

`IRPFFmpeg.exe` - отдельный standalone asset для небольших обновлений основного приложения.

Начиная с `v1.0.4`, версионные релизы должны включать только файлы проекта. Не публикуйте новый большой support-архив при изменении `.exe`, `Language/*.lng`, документации, загрузчика или других файлов проекта.

## DLL-Support Архив

Состав `IRPFFmpeg-support-win-x64.zip`:

```text
IRPFFmpeg-support-win-x64.zip
  heap_dll/
    avcodec-62.dll
    avfilter-11.dll
    avformat-62.dll
    avutil-60.dll
    jpeg62.dll
    libpng16.dll
    SDL2.dll
    SDL2_image.dll
    swresample-6.dll
    swscale-9.dll
    turbojpeg.dll
    zlib1.dll
```

Не добавляйте в этот архив `Start_IRPFFmpeg.exe`, `IRPFFmpeg.exe`, `Language`, `playlist.m3u`, `app.dat`, README, docs или другие файлы проекта.

## Project Архив

Состав `IRPFFmpeg-vX.Y.Z-project-win-x64.zip`:

```text
IRPFFmpeg-vX.Y.Z-project-win-x64.zip
  Start_IRPFFmpeg.exe
  IRPFFmpeg.exe
  LICENSE
  THIRD_PARTY_NOTICES.md
  README.md
  README_ENG.md
  CHANGELOG.md
  Language/
    english.lng
    russian.lng
  docs/
    ...
```

Опционально можно добавить `playlist.m3u`, только если это намеренно подготовленный публичный стартовый плейлист. Не добавляйте личные рабочие плейлисты.

Обычно не добавляйте `app.dat`: приложение должно стартовать и после удаления этого файла. Если `app.dat` все же нужен как стартовое состояние, перед публикацией проверьте, что в нем нет личных или тестовых данных.

Не добавляйте в project-архив `heap_dll` и сторонние DLL.

## Runtime Layout

Минимальная рабочая папка пользователя после распаковки DLL-support и project-архивов:

```text
Start_IRPFFmpeg.exe
IRPFFmpeg.exe
heap_dll/
  avcodec-62.dll
  avfilter-11.dll
  avformat-62.dll
  avutil-60.dll
  jpeg62.dll
  libpng16.dll
  SDL2.dll
  SDL2_image.dll
  swresample-6.dll
  swscale-9.dll
  turbojpeg.dll
  zlib1.dll
Language/
  english.lng
  russian.lng
LICENSE
THIRD_PARTY_NOTICES.md
```

Во время работы приложение может создать:

```text
app.dat
playlist.m3u
Rec/
cover_cache/
debug_log.txt
```

Эти runtime-файлы не должны попадать в git и не должны автоматически попадать в project-архив.

## Не Включать

Не включайте в релизные архивы:

- `.pdb`, `.obj`, `.tlog`, `.log`;
- `.vs/`;
- `cover_cache/`;
- `Rec/`;
- `heap_dll` внутри project-архива;
- файлы проекта внутри DLL-support архива;
- личные плейлисты и пользовательские настройки.

## Лицензии

Код IRPFFmpeg распространяется под MIT License. Файл `LICENSE` должен быть рядом с приложением в project-архиве.

Сторонние DLL распространяются по своим лицензиям:

- FFmpeg DLL - LGPL/GPL согласно конкретной сборке FFmpeg;
- SDL2 и SDL2_image - zlib license;
- libpng, zlib, jpeg/libjpeg-turbo и другие runtime-компоненты - согласно их собственным лицензиям.

Для FFmpeg важно сохранить динамическую линковку через DLL и приложить информацию о конкретной сборке: license text, build configuration и соответствующий source offer/source archive, если публикуется бинарный релиз.

Подробности собраны в `THIRD_PARTY_NOTICES.md`.

## Старый Формат

Старый формат `IRPFFmpeg-vX.Y.Z-support-win-x64.zip`, где в одном архиве лежали DLL и файлы проекта, больше не использовать. Он зря занимал место на GitHub при каждом изменении файлов проекта, хотя сторонние DLL не менялись.

## Текст Релиза

Шаблон:

```markdown
## IRPFFmpeg X.Y.Z

### Что нового
- ...

### Исправления
- ...

### Как запускать
Для новой установки распакуйте `IRPFFmpeg-support-win-x64.zip` и `IRPFFmpeg-vX.Y.Z-project-win-x64.zip` в одну папку, затем запустите `Start_IRPFFmpeg.exe`.

Для обновления существующей установки с уже установленным `heap_dll` распакуйте только project-архив или замените standalone `IRPFFmpeg.exe`, если релиз меняет только основное приложение.

### Требования
- Windows 10/11 x64.

### Известные ограничения
- Лицензия FFmpeg зависит от конкретной сборки DLL.
```

## Проверка Перед Публикацией

Проверьте архивы на чистой папке:

1. Распакуйте DLL-support архив.
2. Распакуйте project-архив в ту же папку.
3. Запустите `Start_IRPFFmpeg.exe`.
4. Убедитесь, что нет сообщения о недостающих DLL.
5. Воспроизведите поток.
6. Закройте приложение и проверьте повторный запуск.
