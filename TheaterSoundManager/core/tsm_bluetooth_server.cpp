#include "tsm_bluetooth_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include <string.h>
#include <thread>
#include <iostream>

#include "tsm_playlist_manager.h"
#include "tsm_ui_manager.h"

#include <spdlog/spdlog.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

static const GUID serviceGuid = { 0x00001101, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };

ULONGLONG GetLocalBluetoothAddress()
{
    HANDLE hRadio = NULL;
    BLUETOOTH_RADIO_INFO radioInfo;
    radioInfo.dwSize = sizeof(BLUETOOTH_RADIO_INFO);
    BLUETOOTH_FIND_RADIO_PARAMS findRadioParams = { sizeof(BLUETOOTH_FIND_RADIO_PARAMS) };
    HANDLE hFind = BluetoothFindFirstRadio(&findRadioParams, &hRadio);
    if (hFind == NULL)
    {
        spdlog::error("No Bluetooth adapter found.");
        return 0;
    }
    if (BluetoothGetRadioInfo(hRadio, &radioInfo) != ERROR_SUCCESS)
    {
        spdlog::error("Failed to get Bluetooth adapter info.");
        CloseHandle(hRadio);
        BluetoothFindRadioClose(hFind);
        return 0;
    }
    ULONGLONG btAddr = radioInfo.address.ullLong;
    CloseHandle(hRadio);
    BluetoothFindRadioClose(hFind);
    return btAddr;
}

int RegisterBluetoothService(SOCKET sock, SOCKADDR_BTH* localBthAddr)
{
    CSADDR_INFO csAddrInfo;
    ZeroMemory(&csAddrInfo, sizeof(csAddrInfo));
    csAddrInfo.iSocketType = SOCK_STREAM;
    csAddrInfo.iProtocol = BTHPROTO_RFCOMM;
    csAddrInfo.LocalAddr.lpSockaddr = (LPSOCKADDR)localBthAddr;
    csAddrInfo.LocalAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);
    csAddrInfo.RemoteAddr.lpSockaddr = (LPSOCKADDR)localBthAddr;
    csAddrInfo.RemoteAddr.iSockaddrLength = sizeof(SOCKADDR_BTH);

    WSAQUERYSET wsaQuerySet;
    ZeroMemory(&wsaQuerySet, sizeof(wsaQuerySet));
    wsaQuerySet.dwSize = sizeof(WSAQUERYSET);

    WCHAR serviceName[] = L"TheaterSound";
    wsaQuerySet.lpszServiceInstanceName = serviceName;
    wsaQuerySet.lpServiceClassId = (GUID*)&serviceGuid;
    wsaQuerySet.dwNameSpace = NS_BTH;
    wsaQuerySet.dwNumberOfCsAddrs = 1;
    wsaQuerySet.lpcsaBuffer = &csAddrInfo;

    BYTE sdpRecord[] = {
        0x35, 0x19,
        0x09, 0x00, 0x01,
        0x35, 0x03,
        0x19, 0x11, 0x01,
        0x09, 0x01, 0x00,
        0x25, 0x0C,
        'T','h','e','a','t','e','r','S','o','u','n','d'
    };

    BLOB serviceRecordBlob;
    serviceRecordBlob.cbSize = sizeof(sdpRecord);
    serviceRecordBlob.pBlobData = sdpRecord;
    wsaQuerySet.lpBlob = &serviceRecordBlob;

    int ret = WSASetService(&wsaQuerySet, RNRSERVICE_REGISTER, 0);
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("WSASetService failed ({}).", err);
        return err;
    }
    
    // Convert wide string to narrow string for spdlog
    char serviceNameNarrow[32];
    wcstombs(serviceNameNarrow, serviceName, sizeof(serviceNameNarrow));
    spdlog::info("Service registered with name '{}'.", serviceNameNarrow);
    return 0;
}

void processCommand(SOCKET clientSocket, char* buffer)
{
    buffer[strcspn(buffer, "\r\n")] = 0;
    
    if (strcmp(buffer, "PLAY") == 0)
    {
        TSM::PlaylistManager::GetInstance().PlayFromIndex("playlist_test", 0);
        const char* response = "Playing";
        send(clientSocket, response, (int)strlen(response), 0);
    }
    else if (strcmp(buffer, "STOP") == 0)
    {
        TSM::PlaylistManager::GetInstance().Stop("playlist_test");
        const char* response = "Stopped";
        send(clientSocket, response, (int)strlen(response), 0);
    }
    else if (strcmp(buffer, "NEXT") == 0)
    {
        TSM::PlaylistManager::GetInstance().Stop("playlist_test");
        TSM::PlaylistManager::GetInstance().PlayFromIndex("playlist_test", 1);
        const char* response = "Next track";
        send(clientSocket, response, (int)strlen(response), 0);
    }
    else if (strncmp(buffer, "SET_VOLUME", 10) == 0)
    {
        float volume = 0.5f;
        if (sscanf(buffer, "SET_VOLUME %f", &volume) == 1)
        {
            TSM::UIManager::GetInstance().SetMusicVolume(volume);
            TSM::UIManager::GetInstance().ForceUpdateAllVolumes();
            const char* response = "Volume set";
            send(clientSocket, response, (int)strlen(response), 0);
        }
        else
        {
            const char* response = "Invalid volume command";
            send(clientSocket, response, (int)strlen(response), 0);
        }
    }
    else
    {
        const char* response = "Unknown command";
        send(clientSocket, response, (int)strlen(response), 0);
    }
}

void BluetoothServerLoop()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (result != 0)
    {
        spdlog::error("WSAStartup failed ({}).", result);
        return;
    }
    
    ULONGLONG btAddr = GetLocalBluetoothAddress();
    if (btAddr == 0)
    {
        spdlog::error("Invalid or disabled Bluetooth adapter.");
        WSACleanup();
        return;
    }
    spdlog::info("Local Bluetooth address: 0x{}.", btAddr);
    
    SOCKET serverSocket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (serverSocket == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        spdlog::error("socket() failed ({}).", err);
        WSACleanup();
        return;
    }
    
    SOCKADDR_BTH localAddr;
    ZeroMemory(&localAddr, sizeof(localAddr));
    localAddr.addressFamily = AF_BTH;
    localAddr.btAddr = btAddr;
    localAddr.port = BT_PORT_ANY;
    
    if (bind(serverSocket, (SOCKADDR*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("bind() failed ({}).", err);
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    int addrLen = sizeof(localAddr);
    if (getsockname(serverSocket, (SOCKADDR*)&localAddr, &addrLen) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("getsockname() failed ({}).", err);
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    spdlog::info("Server listening on RFCOMM port: {}.", localAddr.port);
    
    if (RegisterBluetoothService(serverSocket, &localAddr) != 0)
    {
        spdlog::error("Failed to register service.");
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    if (listen(serverSocket, 1) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("listen() failed ({}).", err);
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    spdlog::info("Waiting for Bluetooth connection...");
    
    while (true)
    {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            int err = WSAGetLastError();
            spdlog::error("accept() failed ({}).", err);
            break;
        }
        spdlog::info("Client connected.");
        
        const int bufferSize = 1024;
        char buffer[bufferSize];
        int bytesReceived;
        do {
            bytesReceived = recv(clientSocket, buffer, bufferSize - 1, 0);
            if (bytesReceived > 0)
            {
                buffer[bytesReceived] = '\0';
                spdlog::info("Received: {}", buffer);
                processCommand(clientSocket, buffer);
            }
            else if (bytesReceived == 0)
            {
                spdlog::info("Client disconnected.");
            }
            else
            {
                int err = WSAGetLastError();
                spdlog::error("recv() failed ({}).", err);
                break;
            }
        } while (bytesReceived > 0);
        
        closesocket(clientSocket);
        spdlog::info("Waiting for new connection...");
    }
    
    closesocket(serverSocket);
    WSACleanup();
    spdlog::info("Bluetooth server stopped.");
}

void StartBluetoothServer()
{
    std::thread btThread(BluetoothServerLoop);
    btThread.detach();
}
