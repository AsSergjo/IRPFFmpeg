#pragma once
#define NOMINMAX
#include <string>
#include <vector>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_syswm.h>
#include <atomic>
#include <shlwapi.h>

#ifndef UTIL_H
#define UTIL_H

std::wstring utf8_to_wstring(const std::string& str);
std::string wstring_to_utf8(const std::wstring& wstr);
std::string url_encode(const std::string& value);
std::wstring url_decode(const std::wstring& str);
HRESULT download_file(const std::wstring& url, const std::wstring& file_path, int timeout);
std::wstring ResolveStationNameFromUrl(const std::wstring& url);

#endif // UTIL_H

// Структура для хранения информации о каждом элементе плейлиста
struct PlaylistItem {
    std::wstring name;
    std::wstring url;
};

// Функция для загрузки плейлиста из файла
void loadPlaylist(const std::wstring& filename, std::vector<PlaylistItem>& playlist);

// Functions for saving/loading to/from app.dat
bool loadPlaylistFromDat(const std::wstring& filename, std::vector<PlaylistItem>& playlist, int& selectedIndex);
void savePlaylistToDat(const std::wstring& filename, const std::vector<PlaylistItem>& playlist, int selectedIndex);

// Функция для отрисовки изображения
bool redrawCoverImage(HWND hDlg);
bool initCoverRenderer(HWND hDlg);
bool reloadCoverTexture();
// Logging function
void LogToUI(const std::string& message);
// Функция для очистки ресурсов SDL
void cleanupSDL();
void ResetMetadataCaches();
