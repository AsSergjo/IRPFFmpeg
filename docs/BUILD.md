# Сборка IRPFFmpeg

Эта инструкция описывает локальную сборку проекта из исходников на Windows.

## Требования

- Windows 10 или Windows 11;
- Visual Studio 2022;
- MSVC toolset v143;
- Windows SDK 10;
- C++17;
- FFmpeg development build;
- SDL2;
- SDL2_image.

Основная рабочая конфигурация проекта - `Release|x64`. Конфигурации Win32 присутствуют в решении, но сторонние зависимости сейчас настроены для x64.

## Зависимости

Проект `IRPFFmpeg` использует FFmpeg для сетевых потоков, декодирования, фильтров, ресемплинга и записи. Также используются SDL2/SDL2_image и системные библиотеки Windows.

Текущие пути в `IRPFFmpeg.vcxproj`:

```text
D:\Code\ffmpeg-dev\include
D:\Code\ffmpeg-dev\lib
D:\Code\ffmpeg-dev\bin
C:\dev\vcpkg\installed\x64-windows\include\SDL2
C:\dev\vcpkg\installed\x64-windows\lib
```

Если на вашей машине зависимости лежат в других каталогах, откройте свойства проекта `IRPFFmpeg` и обновите:

- `C/C++ > General > Additional Include Directories`;
- `Linker > General > Additional Library Directories`;
- пользовательский макрос `FfmpegBinDir`, если используете автокопирование FFmpeg DLL.

## Сборка в Visual Studio

1. Откройте `IRPFFmpeg.sln`.
2. Выберите конфигурацию `Release` и платформу `x64`.
3. Соберите решение командой `Build > Build Solution`.
4. Проверьте папку `x64\Release`.

В решении два проекта:

- `IRPFFmpeg` - основное приложение;
- `Start_IRPFFmpeg` - загрузчик, зависит от основного приложения.

`Start_IRPFFmpeg` собирается после `IRPFFmpeg` и должен быть основной точкой запуска для пользователя.

## Что должно появиться после сборки

Ожидаемый runtime-состав:

```text
x64\Release\
  Start_IRPFFmpeg.exe
  IRPFFmpeg.exe
  heap_dll\
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

Загрузчик проверяет эти DLL при старте. Если какой-то файл отсутствует, пользователь увидит сообщение со списком недостающих библиотек.

## Командная сборка

Из Developer PowerShell for Visual Studio:

```powershell
msbuild IRPFFmpeg.sln /p:Configuration=Release /p:Platform=x64
```

Если `msbuild` не найден, запустите команду из Developer PowerShell или Developer Command Prompt, которые устанавливаются вместе с Visual Studio.

## Частые проблемы

`Cannot open include file`

Проверьте пути к FFmpeg и SDL2 в настройках проекта.

`LNK1104` или `cannot open file *.lib`

Проверьте `Additional Library Directories` и наличие `.lib` файлов FFmpeg/SDL2 для x64.

Приложение не стартует после сборки

Запускайте `Start_IRPFFmpeg.exe`, а не `IRPFFmpeg.exe`. Убедитесь, что рядом есть папка `heap_dll` с runtime DLL.

Отсутствуют FFmpeg DLL в `heap_dll`

Проверьте значение `FfmpegBinDir` в `IRPFFmpeg.vcxproj`. Сейчас оно указывает на `D:\Code\ffmpeg-dev\bin`.

## Лицензии зависимостей

Код IRPFFmpeg распространяется под MIT License. Сторонние библиотеки сохраняют свои лицензии.

Особенно внимательно относитесь к FFmpeg: лицензия DLL зависит от параметров сборки. Для LGPL-сценария используйте динамическую линковку через DLL и сборку FFmpeg без `--enable-gpl` и без `--enable-nonfree`.

Для бинарных релизов добавляйте `THIRD_PARTY_NOTICES.md` и license texts для всех DLL.
