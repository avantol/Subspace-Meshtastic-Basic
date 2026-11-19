#pragma once
// Qt 6 and Qt 5.15 differences. Qt 5.15 builds the 32-bit Windows version (Qt 6 has none).
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#define JS8_AGENT_ERROR &QBluetoothDeviceDiscoveryAgent::errorOccurred
#define JS8_CTL_ERROR &QLowEnergyController::errorOccurred
#define JS8_SVC_ERROR &QLowEnergyService::errorOccurred
#define JS8_SVC_DISCOVERED QLowEnergyService::RemoteServiceDiscovered
#else
#define JS8_AGENT_ERROR QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::error)
#define JS8_CTL_ERROR QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error)
#define JS8_SVC_ERROR QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error)
#define JS8_SVC_DISCOVERED QLowEnergyService::ServiceDiscovered
#endif
