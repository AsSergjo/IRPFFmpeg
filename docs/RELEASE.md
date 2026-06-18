# Подготовка релиза

Готовые бинарные сборки лучше публиковать через GitHub Releases, а не хранить в репозитории.

## Перед сборкой

1. Соберите `Release|x64`.
2. Запустите `Start_IRPFFmpeg.exe` из `x64\Release`.
3. Проверьте воспроизведение хотя бы одной станции из `playlist.m3u`.
4. Проверьте старт без Visual Studio.
5. Проверьте запись короткого фрагмента в MP3.
6. Проверьте запись короткого фрагмента в FLAC, если менялась логика записи.
7. Проверьте, что приложение стартует после удаления `app.dat`.

## Состав архива

Рекомендуемый архив:

```text
IRPFFmpeg-vX.Y.Z-win-x64.zip
  Start_IRPFFmpeg.exe
  IRPFFmpeg.exe
  LICENSE
  THIRD_PARTY_NOTICES.md
  playlist.m3u
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
  licenses/
    FFmpeg-LICENSE.txt
    FFmpeg-build-configuration.txt
    SDL2-LICENSE.txt
    SDL2_image-LICENSE.txt
    libpng-LICENSE.txt
    zlib-LICENSE.txt
    libjpeg-turbo-LICENSE.txt
```

Не включайте в архив:

- `.pdb`, `.obj`, `.tlog`, `.log`;
- `.vs/`;
- `app.dat`;
- `cover_cache/`;
- `Rec/`;
- личные плейлисты, если они не должны быть публичными.

## Лицензии в релизе

Код IRPFFmpeg распространяется под MIT License. Файл `LICENSE` должен быть рядом с приложением.

Сторонние DLL распространяются по своим лицензиям:

- FFmpeg DLL - LGPL/GPL согласно конкретной сборке FFmpeg;
- SDL2 и SDL2_image - zlib license;
- libpng, zlib, jpeg/libjpeg-turbo и другие runtime-компоненты - согласно их собственным лицензиям.

Для FFmpeg важно сохранить динамическую линковку через DLL и приложить информацию о конкретной сборке: license text, build configuration и соответствующий source offer/source archive, если публикуется бинарный релиз.

Подробности собраны в `THIRD_PARTY_NOTICES.md`.

## Текст релиза

Шаблон:

```markdown
## IRPFFmpeg X.Y.Z

### Что нового
- ...

### Исправления
- ...

### Как запускать
Распакуйте архив в отдельную папку и запустите `Start_IRPFFmpeg.exe`.

### Требования
- Windows 10/11 x64.

### Известные ограничения
- Лицензия FFmpeg зависит от конкретной сборки DLL.
```

## Проверка перед публикацией

Проверьте архив на чистой папке:

1. Распакуйте архив.
2. Запустите `Start_IRPFFmpeg.exe`.
3. Убедитесь, что нет сообщения о недостающих DLL.
4. Воспроизведите поток.
5. Закройте приложение и проверьте повторный запуск.
