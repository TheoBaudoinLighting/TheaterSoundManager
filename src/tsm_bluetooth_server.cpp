#include "tsm_bluetooth_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <initguid.h>
#include <string.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <thread>
#include <utility>
#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

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
std::mutex g_stateMutex;
std::mutex g_lifecycleMutex;
std::condition_variable g_stateChanged;
std::thread g_bluetoothThread;
std::atomic<bool> g_serverRunning{false};
BluetoothServerStatus g_serverStatus;
SOCKET g_serverSocket = INVALID_SOCKET;
SOCKET g_clientSocket = INVALID_SOCKET;
constexpr std::size_t MaximumPendingCommands = 256;
constexpr std::size_t MaximumCommandBytes = 4096;

void SetServerStatus(BluetoothServerState state, std::string error = {})
{
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_serverStatus = {state, std::move(error)};
    }
    g_stateChanged.notify_all();
}

void FailServer(std::string error)
{
    g_serverRunning = false;
    SetServerStatus(BluetoothServerState::Failed, std::move(error));
}

void ClearPendingCommands()
{
    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_pendingCommands.clear();
}

bool QueueCommand(BluetoothCommandType type, float value = 0.0f)
{
    std::lock_guard<std::mutex> lock(g_commandMutex);
    if (g_pendingCommands.size() >= MaximumPendingCommands) return false;
    g_pendingCommands.push_back({type, value});
    return true;
}

bool SendResponse(SOCKET clientSocket, std::string_view response)
{
    std::string framed(response);
    framed.push_back('\n');
    std::size_t sent = 0;
    while (sent < framed.size())
    {
        const int result = send(
            clientSocket,
            framed.data() + sent,
            static_cast<int>(framed.size() - sent),
            0);
        if (result == SOCKET_ERROR || result == 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void QueueOrRespond(
    SOCKET clientSocket,
    BluetoothCommandType type,
    std::string_view successResponse,
    float value = 0.0f)
{
    SendResponse(
        clientSocket,
        QueueCommand(type, value) ? successResponse : "Command queue full");
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

int RegisterBluetoothService(SOCKADDR_BTH* localBthAddr)
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

bool ParseVolumeCommand(const std::string& command, float& volume)
{
    constexpr std::string_view prefix = "SET_VOLUME ";
    if (!command.starts_with(prefix)) return false;

    const char* begin = command.c_str() + prefix.size();
    char* end = nullptr;
    errno = 0;
    volume = std::strtof(begin, &end);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return end != begin && end && *end == '\0' && errno != ERANGE &&
           std::isfinite(volume) && volume >= 0.0f && volume <= 1.0f;
}

void ProcessCommand(SOCKET clientSocket, const std::string& command)
{
    if (command == "PLAY")
    {
        QueueOrRespond(clientSocket, BluetoothCommandType::Play, "Play queued");
    }
    else if (command == "PLAY_RANDOM")
    {
        QueueOrRespond(
            clientSocket, BluetoothCommandType::PlayRandom, "Random playback queued");
    }
    else if (command == "STOP")
    {
        QueueOrRespond(clientSocket, BluetoothCommandType::Stop, "Stop queued");
    }
    else if (command == "NEXT")
    {
        QueueOrRespond(clientSocket, BluetoothCommandType::Next, "Next track queued");
    }
    else if (command == "WEDDING_PHASE1")
    {
        QueueOrRespond(
            clientSocket, BluetoothCommandType::WeddingPhase1, "Wedding phase 1 queued");
    }
    else if (command == "WEDDING_PHASE2")
    {
        QueueOrRespond(
            clientSocket, BluetoothCommandType::WeddingPhase2, "Wedding phase 2 queued");
    }
    else if (command == "WEDDING_PHASE3")
    {
        QueueOrRespond(
            clientSocket, BluetoothCommandType::WeddingPhase3, "Wedding phase 3 queued");
    }
    else if (command == "NEXT_PHASE")
    {
        QueueOrRespond(
            clientSocket, BluetoothCommandType::NextPhase, "Next wedding phase queued");
    }
    else if (command.starts_with("SET_VOLUME"))
    {
        float volume = 0.0f;
        if (ParseVolumeCommand(command, volume))
        {
            QueueOrRespond(
                clientSocket,
                BluetoothCommandType::SetVolume,
                "Volume change queued",
                volume);
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
        const std::string error = "WSAStartup failed (" + std::to_string(result) + ").";
        spdlog::error("{}", error);
        FailServer(error);
        return;
    }
    
    ULONGLONG btAddr = GetLocalBluetoothAddress();
    if (btAddr == 0)
    {
        const std::string error = "No enabled Bluetooth adapter is available.";
        spdlog::error("{}", error);
        WSACleanup();
        FailServer(error);
        return;
    }
    spdlog::info("Local Bluetooth address: 0x{}.", btAddr);
    
    SOCKET serverSocket = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (serverSocket == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        const std::string error = "Bluetooth socket creation failed (" +
            std::to_string(err) + ").";
        spdlog::error("{}", error);
        WSACleanup();
        FailServer(error);
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
        const std::string error = "Bluetooth bind failed (" +
            std::to_string(err) + ").";
        spdlog::error("{}", error);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        FailServer(error);
        return;
    }
    
    int addrLen = sizeof(localAddr);
    if (getsockname(serverSocket, (SOCKADDR*)&localAddr, &addrLen) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        const std::string error = "Bluetooth endpoint query failed (" +
            std::to_string(err) + ").";
        spdlog::error("{}", error);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        FailServer(error);
        return;
    }
    spdlog::info("Server listening on RFCOMM port: {}.", localAddr.port);
    
    if (RegisterBluetoothService(&localAddr) != 0)
    {
        const std::string error = "Bluetooth service registration failed.";
        spdlog::error("{}", error);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        FailServer(error);
        return;
    }
    
    if (listen(serverSocket, 1) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        const std::string error = "Bluetooth listen failed (" +
            std::to_string(err) + ").";
        spdlog::error("{}", error);
        ReleaseTrackedSocket(serverSocket, false);
        WSACleanup();
        FailServer(error);
        return;
    }
    SetServerStatus(BluetoothServerState::Running);
    spdlog::info("Waiting for Bluetooth connection...");
    
    while (g_serverRunning)
    {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            if (!g_serverRunning) break;
            int err = WSAGetLastError();
            const std::string error = "Bluetooth accept failed (" +
                std::to_string(err) + ").";
            spdlog::error("{}", error);
            FailServer(error);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            if (!g_serverRunning)
            {
                closesocket(clientSocket);
                break;
            }
            g_clientSocket = clientSocket;
        }
        spdlog::info("Client connected.");
        
        char buffer[1024];
        std::string pendingInput;
        int bytesReceived = 0;
        bool closeClient = false;
        do
        {
            bytesReceived = recv(
                clientSocket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (bytesReceived > 0)
            {
                pendingInput.append(buffer, static_cast<std::size_t>(bytesReceived));
                std::size_t newline = std::string::npos;
                while ((newline = pendingInput.find('\n')) != std::string::npos)
                {
                    std::string command = pendingInput.substr(0, newline);
                    pendingInput.erase(0, newline + 1);
                    if (!command.empty() && command.back() == '\r') command.pop_back();
                    if (command.size() > MaximumCommandBytes)
                    {
                        SendResponse(clientSocket, "Command too large");
                        closeClient = true;
                        break;
                    }
                    if (command.empty()) continue;
                    spdlog::info("Received Bluetooth command: {}", command);
                    ProcessCommand(clientSocket, command);
                }
                if (pendingInput.size() > MaximumCommandBytes)
                {
                    SendResponse(clientSocket, "Command too large");
                    closeClient = true;
                }
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
        } while (bytesReceived > 0 && g_serverRunning && !closeClient);
        
        ReleaseTrackedSocket(clientSocket, true);
        spdlog::info("Waiting for new connection...");
    }
    
    ReleaseTrackedSocket(serverSocket, false);
    WSACleanup();
    g_serverRunning = false;
    if (GetBluetoothServerStatus().state != BluetoothServerState::Failed)
        SetServerStatus(BluetoothServerState::Stopped);
    spdlog::info("Bluetooth server stopped.");
}

namespace
{
void StopBluetoothServerUnlocked()
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

    if (g_bluetoothThread.joinable()) g_bluetoothThread.join();
    ClearPendingCommands();
    SetServerStatus(BluetoothServerState::Stopped);
}
}

bool StartBluetoothServer(std::string& errorMessage)
{
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    errorMessage.clear();
    const BluetoothServerStatus current = GetBluetoothServerStatus();
    if (current.state == BluetoothServerState::Running) return true;
    if (g_bluetoothThread.joinable()) g_bluetoothThread.join();

    ClearPendingCommands();
    SetServerStatus(BluetoothServerState::Starting);
    g_serverRunning = true;
    g_bluetoothThread = std::thread(BluetoothServerLoop);

    std::unique_lock<std::mutex> lock(g_stateMutex);
    const bool completed = g_stateChanged.wait_for(
        lock,
        std::chrono::seconds(10),
        [] { return g_serverStatus.state != BluetoothServerState::Starting; });
    BluetoothServerStatus status = g_serverStatus;
    lock.unlock();

    if (!completed)
    {
        errorMessage = "Bluetooth server startup timed out.";
        StopBluetoothServerUnlocked();
        SetServerStatus(BluetoothServerState::Failed, errorMessage);
        return false;
    }
    if (status.state == BluetoothServerState::Running) return true;

    errorMessage = status.error.empty()
        ? "Bluetooth server failed to start."
        : status.error;
    if (g_bluetoothThread.joinable()) g_bluetoothThread.join();
    return false;
}

BluetoothServerStatus GetBluetoothServerStatus()
{
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_serverStatus;
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
    std::lock_guard<std::mutex> lifecycleLock(g_lifecycleMutex);
    StopBluetoothServerUnlocked();
}

void DiscardPendingBluetoothCommands()
{
    ClearPendingCommands();
}
