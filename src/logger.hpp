#pragma once

#include <windows.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace ColdSteel3 {

/**
 * Logger
 *
 * Thread-safe, silent background file logger for diagnostic tracking.
 * Writes timestamped events to `camera_tweak_debug.log` without polluting
 * stdout/stderr or disrupting the interactive console HUD.
 */
class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void SetLogFilePath(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open()) {
            m_file.close();
        }
        m_logPath = path;
        m_file.open(m_logPath, std::ios::out | std::ios::app);
    }

    std::string GetLogFilePath() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_logPath;
    }

    void Log(const std::string& category, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureOpen();
        if (!m_file.is_open()) {
            return;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);

        m_file << std::setfill('0')
               << "[" << std::setw(4) << st.wYear << "-"
               << std::setw(2) << st.wMonth << "-"
               << std::setw(2) << st.wDay << " "
               << std::setw(2) << st.wHour << ":"
               << std::setw(2) << st.wMinute << ":"
               << std::setw(2) << st.wSecond << "."
               << std::setw(3) << st.wMilliseconds << "] "
               << "[" << category << "] "
               << message << "\n";
        m_file.flush();
    }

    static std::string Hex(uintptr_t val) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << val;
        return oss.str();
    }

    static std::string Float(float val) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << val;
        return oss.str();
    }

private:
    Logger() : m_logPath("camera_tweak_debug.log") {}
    ~Logger() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void EnsureOpen() {
        if (!m_file.is_open() && !m_logPath.empty()) {
            m_file.open(m_logPath, std::ios::out | std::ios::app);
        }
    }

    mutable std::mutex m_mutex;
    std::string m_logPath;
    std::ofstream m_file;
};

} // namespace ColdSteel3
