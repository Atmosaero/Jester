#include "physics.h"
#include <QSettings>
#include <QSysInfo>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif
namespace trickster {
QJsonObject physicsEnvironment() {
    QJsonObject out{{"os", QSysInfo::prettyProductName()},
                    {"architecture", QSysInfo::currentCpuArchitecture()},
                    {"backend", physicsBackendVersion()}};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        out["peakProcessWorkingSetBytes"] = double(memory.PeakWorkingSetSize);
    MEMORYSTATUSEX total{};
    total.dwLength = sizeof(total);
    if (GlobalMemoryStatusEx(&total))
        out["physicalMemoryBytes"] = double(total.ullTotalPhys);
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    out["logicalProcessors"] = int(system.dwNumberOfProcessors);
    QSettings cpu("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                  QSettings::NativeFormat);
    out["cpu"] = cpu.value("ProcessorNameString").toString().trimmed();
#endif
    out["memoryScope"] =
        "peak working set of this process, including earlier tasks and Qt; not isolated task allocation";
    return out;
}
} // namespace trickster
