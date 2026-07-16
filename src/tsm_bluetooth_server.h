#pragma once

#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <string>

enum class BluetoothServerState
{
    Stopped,
    Starting,
    Running,
    Failed
};

struct BluetoothServerStatus
{
    BluetoothServerState state = BluetoothServerState::Stopped;
    std::string error;
};

bool StartBluetoothServer(std::string& errorMessage);
BluetoothServerStatus GetBluetoothServerStatus();
void ProcessPendingBluetoothCommands();
void DiscardPendingBluetoothCommands();
void StopBluetoothServer();
