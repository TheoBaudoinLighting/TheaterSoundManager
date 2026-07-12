#include "tsm_bluetooth_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include <string.h>
#include <thread>
#include <iostream>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "tsm_playlist_manager.h"
#include "tsm_ui_manager.h"

#include <spdlog/spdlog.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

static const GUID serviceGuid = { 0x00001101, 0x0000, 0x1000, {0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB} };

namespace
{
enum class BluetoothCommandType
{
    Play,
    PlayRandom,
    Stop,
    Next,
    WeddingPhase1,
    WeddingPhase2,
    WeddingPhase3,
    NextPhase,
    SetVolume
};

struct BluetoothCommand
{
    BluetoothCommandType type;
    float value = 0.0f;
};

std::mutex g_commandMutex;
std::deque<BluetoothCommand> g_pendingCommands;
std::mutex g_socketMutex;
std::thread g_bluetoothThread;
std::atomic<bool> g_serverRunning{false};
SOCKET g_serverSocket = INVALID_SOCKET;
SOCKET g_clientSocket = INVALID_SOCKET;

void QueueCommand(BluetoothCommandType type, float value = 0.0f)
{
    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_pendingCommands.push_back({type, value});
}

void SendResponse(SOCKET clientSocket, const char* response)
{
    send(clientSocket, response, static_cast<int>(strlen(response)), 0);
}

void ReleaseTrackedSocket(SOCKET socket, bool client)
{
    bool shouldClose = false;
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        SOCKET& tracked = client ? g_clientSocket : g_serverSocket;
        if (tracked == socket)
        {
            tracked = INVALID_SOCKET;
            shouldClose = true;
        }
    }
    if (shouldClose)
    {
        closesocket(socket);
    }
}
}

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
    
    const int utf8Size = WideCharToMultiByte(
        CP_UTF8, 0, serviceName, -1, nullptr, 0, nullptr, nullptr);
    std::string serviceNameUtf8(
        utf8Size > 0 ? static_cast<std::size_t>(utf8Size) : 1, '\0');
    if (utf8Size > 0)
    {
        WideCharToMultiByte(
            CP_UTF8, 0, serviceName, -1, serviceNameUtf8.data(), utf8Size, nullptr, nullptr);
        serviceNameUtf8.resize(static_cast<std::size_t>(utf8Size - 1));
    }
    spdlog::info("Service registered with name '{}'.", serviceNameUtf8);
    return 0;
}

void processCommand(SOCKET clientSocket, char* buffer)
{
    buffer[strcspn(buffer, "\r\n")] = 0;
    
    if (strcmp(buffer, "PLAY") == 0)
    {
        QueueCommand(BluetoothCommandType::Play);
        SendResponse(clientSocket, "Play queued");
    }
    else if (strcmp(buffer, "PLAY_RANDOM") == 0)
    {
        QueueCommand(BluetoothCommandType::PlayRandom);
        SendResponse(clientSocket, "Random playback queued");
    }
    else if (strcmp(buffer, "STOP") == 0)
    {
        QueueCommand(BluetoothCommandType::Stop);
        SendResponse(clientSocket, "Stop queued");
    }
    else if (strcmp(buffer, "NEXT") == 0)
    {
        QueueCommand(BluetoothCommandType::Next);
        SendResponse(clientSocket, "Next track queued");
    }
    else if (strcmp(buffer, "WEDDING_PHASE1") == 0)
    {
        QueueCommand(BluetoothCommandType::WeddingPhase1);
        SendResponse(clientSocket, "Wedding phase 1 queued");
    }
    else if (strcmp(buffer, "WEDDING_PHASE2") == 0)
    {
        QueueCommand(BluetoothCommandType::WeddingPhase2);
        SendResponse(clientSocket, "Wedding phase 2 queued");
    }
    else if (strcmp(buffer, "WEDDING_PHASE3") == 0)
    {
        QueueCommand(BluetoothCommandType::WeddingPhase3);
        SendResponse(clientSocket, "Wedding phase 3 queued");
    }
    else if (strcmp(buffer, "NEXT_PHASE") == 0)
    {
        QueueCommand(BluetoothCommandType::NextPhase);
        SendResponse(clientSocket, "Next wedding phase queued");
    }
    else if (strncmp(buffer, "SET_VOLUME", 10) == 0)
    {
        float volume = 0.5f;
        if (sscanf_s(buffer, "SET_VOLUME %f", &volume) == 1)
        {
            QueueCommand(BluetoothCommandType::SetVolume, volume);
            SendResponse(clientSocket, "Volume change queued");
        }
        else
        {
            SendResponse(clientSocket, "Invalid volume command");
        }
    }
    else
    {
        SendResponse(clientSocket, "Unknown command");
    }
}

void BluetoothServerLoop()
{
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (result != 0)
    {
        spdlog::error("WSAStartup failed ({}).", result);
        g_serverRunning = false;
        return;
    }
    
    ULONGLONG btAddr = GetLocalBluetoothAddress();
    if (btAddr == 0)
    {
        spdlog::error("Invalid or disabled Bluetooth adapter.");
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    spdlog::info("Local Bluetooth address: 0x{}.", btAddr);
    
    SOCKET serverSocket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (serverSocket == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        spdlog::error("socket() failed ({}).", err);
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        g_serverSocket = serverSocket;
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
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    
    int addrLen = sizeof(localAddr);
    if (getsockname(serverSocket, (SOCKADDR*)&localAddr, &addrLen) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("getsockname() failed ({}).", err);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    spdlog::info("Server listening on RFCOMM port: {}.", localAddr.port);
    
    if (RegisterBluetoothService(serverSocket, &localAddr) != 0)
    {
        spdlog::error("Failed to register service.");
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    
    if (listen(serverSocket, 1) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        spdlog::error("listen() failed ({}).", err);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        g_serverRunning = false;
        return;
    }
    spdlog::info("Waiting for Bluetooth connection...");
    
    while (g_serverRunning)
    {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            if (!g_serverRunning) break;
            int err = WSAGetLastError();
            spdlog::error("accept() failed ({}).", err);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_clientSocket = clientSocket;
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
        } while (bytesReceived > 0 && g_serverRunning);
        
        ReleaseTrackedSocket(clientSocket, true);
        spdlog::info("Waiting for new connection...");
    }
    
    ReleaseTrackedSocket(serverSocket, false);
    WSACleanup();
    g_serverRunning = false;
    spdlog::info("Bluetooth server stopped.");
}

void StartBluetoothServer()
{
    if (g_serverRunning || g_bluetoothThread.joinable())
        return;

    g_serverRunning = true;
    g_bluetoothThread = std::thread(BluetoothServerLoop);
}

void ProcessPendingBluetoothCommands()
{
    std::deque<BluetoothCommand> commands;
    {
        std::lock_guard<std::mutex> lock(g_commandMutex);
        commands.swap(g_pendingCommands);
    }

    constexpr const char* preShowPlaylistName = "playlist_PreShow";
    for (const BluetoothCommand& command : commands)
    {
        auto& playlistManager = TSM::PlaylistManager::GetInstance();
        auto& uiManager = TSM::UIManager::GetInstance();
        switch (command.type)
        {
            case BluetoothCommandType::Play:
                if (auto* playlist = playlistManager.GetPlaylistByName(preShowPlaylistName))
                    playlistManager.Play(preShowPlaylistName, playlist->options);
                break;
            case BluetoothCommandType::PlayRandom:
                uiManager.PlayRandomMusic();
                break;
            case BluetoothCommandType::Stop:
                uiManager.StopAllMusic();
                break;
            case BluetoothCommandType::Next:
                playlistManager.SkipToNextTrack(preShowPlaylistName);
                break;
            case BluetoothCommandType::WeddingPhase1:
                uiManager.StartWeddingPhase1(true);
                break;
            case BluetoothCommandType::WeddingPhase2:
                uiManager.StartWeddingPhase2(true);
                break;
            case BluetoothCommandType::WeddingPhase3:
                uiManager.StartWeddingPhase3();
                break;
            case BluetoothCommandType::NextPhase:
                uiManager.NextWeddingPhase();
                break;
            case BluetoothCommandType::SetVolume:
                uiManager.SetMusicVolume(command.value);
                uiManager.ForceUpdateAllVolumes();
                break;
        }
    }
}

void StopBluetoothServer()
{
    g_serverRunning = false;
    {
        std::lock_guard<std::mutex> lock(g_socketMutex);
        if (g_clientSocket != INVALID_SOCKET)
        {
            shutdown(g_clientSocket, SD_BOTH);
            closesocket(g_clientSocket);
            g_clientSocket = INVALID_SOCKET;
        }
        if (g_serverSocket != INVALID_SOCKET)
        {
            shutdown(g_serverSocket, SD_BOTH);
            closesocket(g_serverSocket);
            g_serverSocket = INVALID_SOCKET;
        }
    }

    if (g_bluetoothThread.joinable())
    {
        g_bluetoothThread.join();
    }
}
