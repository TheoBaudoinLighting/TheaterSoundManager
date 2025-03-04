#pragma once

// Nécessaire pour les API Bluetooth sous Windows
#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <string>

// Démarre le serveur Bluetooth dans un thread séparé.
void StartBluetoothServer();
