#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include "gui/MainWindow.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Initialize COM for potential future use
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    MainWindow mainWindow;
    if (!mainWindow.create(hInstance)) {
        MessageBoxW(nullptr, L"无法创建主窗口", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    int result = mainWindow.run();

    WSACleanup();
    CoUninitialize();
    return result;
}
