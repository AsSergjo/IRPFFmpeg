#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kTargetExe[] = L"IRPFFmpeg.exe";
constexpr wchar_t kDllDirName[] = L"heap_dll";
constexpr const wchar_t* kRequiredDlls[] = {
    L"avcodec-62.dll",
    L"avfilter-11.dll",
    L"avformat-62.dll",
    L"avutil-60.dll",
    L"jpeg62.dll",
    L"libpng16.dll",
    L"SDL2.dll",
    L"SDL2_image.dll",
    L"swresample-6.dll",
    L"swscale-9.dll",
    L"turbojpeg.dll",
    L"zlib1.dll",
};

std::wstring GetLastErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"Unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    size_t slashCount = 0;

    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashCount;
        } else if (ch == L'"') {
            quoted.append(slashCount * 2 + 1, L'\\');
            quoted.push_back(ch);
            slashCount = 0;
        } else {
            quoted.append(slashCount, L'\\');
            slashCount = 0;
            quoted.push_back(ch);
        }
    }

    quoted.append(slashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring GetExeDirectory() {
    std::vector<wchar_t> path(MAX_PATH);
    DWORD length = 0;

    for (;;) {
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L"";
        }
        if (length < path.size() - 1) {
            break;
        }
        path.resize(path.size() * 2);
    }

    std::wstring result(path.data(), length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

bool EnsureDirectoryExists(const std::wstring& directory, std::wstring& error) {
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            return true;
        }
        error = directory + L" exists, but it is not a directory.";
        return false;
    }

    if (CreateDirectoryW(directory.c_str(), nullptr)) {
        return true;
    }

    const DWORD createError = GetLastError();
    if (createError == ERROR_ALREADY_EXISTS) {
        return true;
    }

    error = L"Could not create " + directory + L": " + GetLastErrorText(createError);
    return false;
}

void MoveRootDllsToHeapDir(const std::wstring& exeDir, const std::wstring& heapDir, std::wstring& warnings) {
    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = JoinPath(exeDir, L"*.dll");
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD findError = GetLastError();
        if (findError != ERROR_FILE_NOT_FOUND && findError != ERROR_PATH_NOT_FOUND) {
            warnings += L"Could not scan loader directory for DLL files: ";
            warnings += GetLastErrorText(findError);
            warnings += L"\n";
        }
        return;
    }

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        const std::wstring fileName = findData.cFileName;
        const std::wstring source = JoinPath(exeDir, fileName);
        const std::wstring target = JoinPath(heapDir, fileName);

        if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            warnings += L"Could not move ";
            warnings += fileName;
            warnings += L": ";
            warnings += GetLastErrorText(GetLastError());
            warnings += L"\n";
        }
    } while (FindNextFileW(find, &findData));

    FindClose(find);
}

std::wstring GetMissingRequiredDlls(const std::wstring& heapDir) {
    std::wstring missing;
    for (const wchar_t* dllName : kRequiredDlls) {
        const std::wstring dllPath = JoinPath(heapDir, dllName);
        const DWORD attributes = GetFileAttributesW(dllPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            missing += L"  ";
            missing += dllName;
            missing += L"\n";
        }
    }
    return missing;
}

std::wstring BuildChildCommandLine(const std::wstring& targetExe) {
    std::wstring commandLine = QuoteArg(targetExe);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return commandLine;
    }

    for (int i = 1; i < argc; ++i) {
        commandLine.push_back(L' ');
        commandLine += QuoteArg(argv[i]);
    }

    LocalFree(argv);
    return commandLine;
}

bool PrependPathForChild(const std::wstring& dllDir) {
    DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring currentPath;
    if (length > 0) {
        std::vector<wchar_t> buffer(length);
        DWORD copied = GetEnvironmentVariableW(L"PATH", buffer.data(), length);
        if (copied > 0 && copied < length) {
            currentPath.assign(buffer.data(), copied);
        }
    }

    std::wstring newPath = dllDir;
    if (!currentPath.empty()) {
        newPath += L";";
        newPath += currentPath;
    }

    return SetEnvironmentVariableW(L"PATH", newPath.c_str()) != FALSE;
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance,
                    _In_opt_ HINSTANCE hPrevInstance,
                    _In_ PWSTR rawCommandLine,
                    _In_ int showCommand) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(rawCommandLine);
    UNREFERENCED_PARAMETER(showCommand);

    const std::wstring exeDir = GetExeDirectory();
    if (exeDir.empty()) {
        MessageBoxW(nullptr, L"Could not determine loader directory.", L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    const std::wstring heapDir = JoinPath(exeDir, kDllDirName);
    const std::wstring targetExe = JoinPath(exeDir, kTargetExe);

    DWORD targetAttributes = GetFileAttributesW(targetExe.c_str());
    if (targetAttributes == INVALID_FILE_ATTRIBUTES || (targetAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring message = L"Could not find ";
        message += kTargetExe;
        message += L" next to the loader.";
        MessageBoxW(nullptr, message.c_str(), L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    std::wstring warnings;
    std::wstring directoryError;
    if (!EnsureDirectoryExists(heapDir, directoryError)) {
        MessageBoxW(nullptr, directoryError.c_str(), L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    MoveRootDllsToHeapDir(exeDir, heapDir, warnings);

    const std::wstring missingDlls = GetMissingRequiredDlls(heapDir);
    if (!missingDlls.empty()) {
        std::wstring message = L"Missing required DLL files in ";
        message += kDllDirName;
        message += L":\n\n";
        message += missingDlls;
        message += L"\nPlace the DLL files in the heap_dll folder next to Start_IRPFFmpeg.exe.";
        if (!warnings.empty()) {
            message += L"\n\nDLL move warnings:\n";
            message += warnings;
        }
        MessageBoxW(nullptr, message.c_str(), L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    if (!PrependPathForChild(heapDir)) {
        std::wstring message = L"Could not update PATH for child process:\n";
        message += GetLastErrorText(GetLastError());
        MessageBoxW(nullptr, message.c_str(), L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    std::wstring commandLine = BuildChildCommandLine(targetExe);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    if (!CreateProcessW(targetExe.c_str(),
                        commandLine.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        exeDir.c_str(),
                        &startupInfo,
                        &processInfo)) {
        std::wstring message = L"Could not start IRPFFmpeg.exe:\n";
        message += GetLastErrorText(GetLastError());
        if (!warnings.empty()) {
            message += L"\n\nDLL move warnings:\n";
            message += warnings;
        }
        MessageBoxW(nullptr, message.c_str(), L"Start_IRPFFmpeg", MB_ICONERROR | MB_OK);
        return 1;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return 0;
}
