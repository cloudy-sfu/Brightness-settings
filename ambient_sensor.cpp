#include "ambient_sensor.h"

#include "app_state.h"
#include "brightness_curve.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include <highlevelmonitorconfigurationapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

class Mcp2221VemlDriver {
    static constexpr DWORD HID_REPORT_SIZE = 65;
    static constexpr BYTE VEML7700_ADDRESS = 0x10;

    HANDLE device_ = INVALID_HANDLE_VALUE;

    bool ExchangeReport(const BYTE* output, BYTE* input) {
        if (!IsOpen()) return false;

        DWORD bytesWritten = 0;
        if (!WriteFile(
                device_, output, HID_REPORT_SIZE, &bytesWritten, nullptr) ||
            bytesWritten != HID_REPORT_SIZE) {
            Close();
            return false;
        }

        std::memset(input, 0, HID_REPORT_SIZE);
        DWORD bytesRead = 0;
        if (!ReadFile(
                device_, input, HID_REPORT_SIZE, &bytesRead, nullptr) ||
            bytesRead != HID_REPORT_SIZE) {
            Close();
            return false;
        }

        if (input[1] != output[1]) {
            Close();
            return false;
        }
        return true;
    }

    bool SendI2CCommand(
        BYTE command,
        BYTE address,
        const BYTE* data,
        uint16_t length
    ) {
        if (!IsOpen() || length > 60) return false;

        for (int retry = 0; retry < 20; ++retry) {
            BYTE report[HID_REPORT_SIZE] = {};
            BYTE response[HID_REPORT_SIZE] = {};
            report[1] = command;
            report[2] = static_cast<BYTE>(length & 0xFF);
            report[3] = static_cast<BYTE>((length >> 8) & 0xFF);
            report[4] = address;
            if (data && length > 0) {
                std::memcpy(&report[5], data, length);
            }

            if (!ExchangeReport(report, response)) return false;
            if (response[2] == 0x00) return true;
            if (response[2] != 0x01) return false;
            Sleep(2);
        }
        return false;
    }

    bool SetI2CSpeed100kHz() {
        if (!IsOpen()) return false;

        BYTE report[HID_REPORT_SIZE] = {};
        BYTE response[HID_REPORT_SIZE] = {};
        report[1] = 0x10;
        report[4] = 0x20;
        report[5] = 118;

        if (!ExchangeReport(report, response)) return false;
        return response[2] == 0x00 &&
               (response[4] == 0x20 || response[4] == 0x00);
    }

    bool I2CCancel() {
        if (!IsOpen()) return false;

        BYTE report[HID_REPORT_SIZE] = {};
        BYTE response[HID_REPORT_SIZE] = {};
        report[1] = 0x10;
        report[3] = 0x10;
        if (!ExchangeReport(report, response)) return false;
        Sleep(2);
        return response[2] == 0x00;
    }

    bool I2CWrite(BYTE address, const BYTE* data, uint16_t length) {
        return SendI2CCommand(0x90, static_cast<BYTE>(address << 1), data, length);
    }

    bool I2CWriteNoStop(BYTE address, const BYTE* data, uint16_t length) {
        return SendI2CCommand(0x94, static_cast<BYTE>(address << 1), data, length);
    }

    bool I2CReadRepeatedStart(BYTE address, uint16_t length) {
        return SendI2CCommand(
            0x93,
            static_cast<BYTE>((address << 1) | 0x01),
            nullptr,
            length);
    }

    bool GetI2CData(BYTE* destination, BYTE requestedLength) {
        if (!IsOpen() || !destination || requestedLength == 0) return false;

        for (int retry = 0; retry < 50; ++retry) {
            BYTE report[HID_REPORT_SIZE] = {};
            BYTE response[HID_REPORT_SIZE] = {};
            report[1] = 0x40;

            if (!ExchangeReport(report, response)) return false;

            const BYTE status = response[2];
            const BYTE receivedLength = response[4];
            if (status == 0x00 &&
                receivedLength != 0x7F &&
                receivedLength >= requestedLength) {
                std::memcpy(destination, &response[5], requestedLength);
                return true;
            }
            if (status != 0x00 && status != 0x41) return false;
            Sleep(2);
        }
        return false;
    }

    bool InitVeml7700() {
        const BYTE configCommand[3] = {0x00, 0x00, 0x00};
        if (!I2CWrite(
                VEML7700_ADDRESS,
                configCommand,
                sizeof(configCommand))) {
            return false;
        }
        Sleep(120);
        return true;
    }

public:
    ~Mcp2221VemlDriver() {
        Close();
    }

    bool IsOpen() const {
        return device_ != INVALID_HANDLE_VALUE;
    }

    void Close() {
        if (IsOpen()) {
            CloseHandle(device_);
            device_ = INVALID_HANDLE_VALUE;
        }
    }

    bool Open() {
        if (IsOpen()) return true;

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO info = SetupDiGetClassDevsW(
            &hidGuid,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE) return false;

        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(interfaceData);

        for (DWORD index = 0;
             SetupDiEnumDeviceInterfaces(
                 info, nullptr, &hidGuid, index, &interfaceData);
             ++index) {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(
                info,
                &interfaceData,
                nullptr,
                0,
                &requiredSize,
                nullptr);
            if (requiredSize == 0) continue;

            std::vector<BYTE> detailBuffer(requiredSize);
            auto* detail =
                reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
                    detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(
                    info,
                    &interfaceData,
                    detail,
                    requiredSize,
                    nullptr,
                    nullptr)) {
                continue;
            }

            HANDLE device = CreateFileW(
                detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (device == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attributes = {};
            attributes.Size = sizeof(attributes);
            const bool isMcp2221 =
                HidD_GetAttributes(device, &attributes) &&
                attributes.VendorID == 0x04D8 &&
                attributes.ProductID == 0x00DD;
            if (!isMcp2221) {
                CloseHandle(device);
                continue;
            }

            device_ = device;
            HidD_FlushQueue(device_);
            if (I2CCancel() && SetI2CSpeed100kHz() && InitVeml7700()) {
                SetupDiDestroyDeviceInfoList(info);
                return true;
            }
            Close();
        }

        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    bool ReadLux(float& lux) {
        if (!IsOpen() && !Open()) return false;

        const BYTE alsRegister = 0x04;
        if (!I2CWriteNoStop(VEML7700_ADDRESS, &alsRegister, 1) ||
            !I2CReadRepeatedStart(VEML7700_ADDRESS, 2)) {
            I2CCancel();
            Close();
            return false;
        }

        BYTE data[2] = {};
        if (!GetI2CData(data, sizeof(data))) {
            I2CCancel();
            Close();
            return false;
        }

        const uint16_t rawCount =
            static_cast<uint16_t>(data[0]) |
            (static_cast<uint16_t>(data[1]) << 8);
        lux = static_cast<float>(rawCount) * 0.0672f;
        return true;
    }
};

void UpdateAutomaticBrightness(float lux) {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_MonitorsMutex);

    for (auto& monitor : g_Monitors) {
        if (!monitor.autoBrightness || !monitor.hasPhysicalHandle) continue;

        if (now - monitor.lastBrightnessReadTick >= READ_INTERVAL_MS) {
            DWORD minimum = 0;
            DWORD current = 0;
            DWORD maximum = 100;
            if (GetMonitorBrightness(
                    monitor.hPhysical,
                    &minimum,
                    &current,
                    &maximum)) {
                monitor.minBrightness = static_cast<int>(minimum);
                monitor.maxBrightness = static_cast<int>(maximum);
                monitor.currentBrightness = static_cast<int>(current);
                monitor.sliderValue = static_cast<int>(current);
            }
            monitor.lastBrightnessReadTick = now;
        }

        int target = static_cast<int>(
            std::round(EvaluateBrightnessCurve(monitor.curve, lux)));
        target = (std::max)(
            monitor.minBrightness,
            (std::min)(monitor.maxBrightness, target));

        if (std::abs(target - monitor.currentBrightness) <
            HYSTERESIS_THRESHOLD_PCT) {
            continue;
        }

        if (SetMonitorBrightness(
                monitor.hPhysical,
                static_cast<DWORD>(target))) {
            monitor.currentBrightness = target;
            monitor.sliderValue = target;
            monitor.lastBrightnessReadTick = now;
        }
    }
}

} // namespace

void SensorPollingWorker() {
    Mcp2221VemlDriver driver;

    while (g_AppRunning.load()) {
        float lux = 0.0f;
        if (driver.ReadLux(lux)) {
            g_SensorConnected.store(true, std::memory_order_relaxed);
            g_CurrentLux.store(lux, std::memory_order_relaxed);
            g_SmoothedLux.store(lux, std::memory_order_relaxed);
            UpdateAutomaticBrightness(lux);
        } else {
            g_SensorConnected.store(false, std::memory_order_relaxed);
        }

        if (WaitForSingleObject(g_hShutdownEvent, READ_INTERVAL_MS) !=
            WAIT_TIMEOUT) {
            break;
        }
    }
}
