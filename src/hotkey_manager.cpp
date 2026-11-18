#include "hotkey_manager.hpp"
#include "logger.hpp"

namespace ColdSteel3 {

HotkeyManager::HotkeyManager() {
    ResetDefaultBindings();
    ResetKeyStateProvider();
}

HotkeyManager::~HotkeyManager() {
    Stop();
}

void HotkeyManager::ResetDefaultBindings() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Toggle Pitch Unlock (360° vertical tilt): F1 or F5
    m_bindings[static_cast<size_t>(HotkeyAction::TogglePitch)] = { VK_F1, VK_F5 };

    // 2. Toggle Zoom Unlock: F2 or F6
    m_bindings[static_cast<size_t>(HotkeyAction::ToggleZoom)] = { VK_F2, VK_F6 };

    // 3. Camera Altitude (Height) Up: F7 or Page Up
    m_bindings[static_cast<size_t>(HotkeyAction::AltitudeUp)] = { VK_F7, VK_PRIOR };

    // 4. Camera Altitude (Height) Down: F8 or Page Down
    m_bindings[static_cast<size_t>(HotkeyAction::AltitudeDown)] = { VK_F8, VK_NEXT };

    // 5. Dynamic Zoom In: '[' (VK_OEM_4) or '=' (VK_OEM_PLUS) — no Insert key required
    m_bindings[static_cast<size_t>(HotkeyAction::ZoomIn)] = { VK_OEM_4, VK_OEM_PLUS };

    // 6. Dynamic Zoom Out: ']' (VK_OEM_6) or '-' (VK_OEM_MINUS) — no Delete/Insert key required
    m_bindings[static_cast<size_t>(HotkeyAction::ZoomOut)] = { VK_OEM_6, VK_OEM_MINUS };

    // 7. Dynamic Pitch Angle Up: F10 (avoids F3/F4 because ed8_3_PC.exe binds F4 to WM_CLOSE!)
    m_bindings[static_cast<size_t>(HotkeyAction::PitchUp)] = { VK_F10, 0 };

    // 8. Dynamic Pitch Angle Down: F11 (avoids F4 which closes ed8_3_PC.exe!)
    m_bindings[static_cast<size_t>(HotkeyAction::PitchDown)] = { VK_F11, 0 };

    // 9. Reset Camera Defaults: F9 or Home
    m_bindings[static_cast<size_t>(HotkeyAction::ResetDefaults)] = { VK_F9, VK_HOME };

    // 10. Clean Detach & Exit: F12 or End
    m_bindings[static_cast<size_t>(HotkeyAction::Exit)] = { VK_F12, VK_END };
}

void HotkeyManager::SetBinding(HotkeyAction action, int primaryKey, int secondaryKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t idx = static_cast<size_t>(action);
    if (idx < m_bindings.size()) {
        m_bindings[idx] = { primaryKey, secondaryKey };
    }
}

KeyBinding HotkeyManager::GetBinding(HotkeyAction action) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t idx = static_cast<size_t>(action);
    if (idx < m_bindings.size()) {
        return m_bindings[idx];
    }
    return { 0, 0 };
}

void HotkeyManager::Start() {
    if (m_running.exchange(true)) {
        return; // Already running
    }
    m_thread = std::thread(&HotkeyManager::PollingThreadLoop, this);
}

void HotkeyManager::Stop() {
    if (m_running.exchange(false)) {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
}

bool HotkeyManager::IsRunning() const {
    return m_running.load(std::memory_order_relaxed);
}

void HotkeyManager::PollingThreadLoop() {
    while (m_running.load(std::memory_order_relaxed)) {
        Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(HOTKEY_POLL_INTERVAL_MS));
    }
}

void HotkeyManager::Update() {
    Update(std::chrono::steady_clock::now());
}

void HotkeyManager::Update(TimePoint now) {
    HotkeyCallbacks callbacksCopy;
    float altitudeStep = 0.0f;
    float zoomStep = 0.0f;
    float pitchStep = 0.0f;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callbacksCopy = m_callbacks;
        altitudeStep = m_altitudeStep;
        zoomStep = m_zoomStep;
        pitchStep = m_pitchStep;
    }

    const float speedMultiplier = CalculateSpeedMultiplier();
    bool stateChanged = false;

    // Helper macro / lambda for handling edge-triggered debounced toggle actions
    auto handleToggleAction = [&](HotkeyAction action, const std::function<void()>& cb) {
        const size_t idx = static_cast<size_t>(action);
        const bool isDown = IsActionKeyDown(action);
        const bool wasDown = m_prevActionDown[idx];
        m_prevActionDown[idx] = isDown;

        if (isDown && !wasDown) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastActionFireTime[idx]
            ).count();

            if (elapsed >= static_cast<int64_t>(TOGGLE_DEBOUNCE_LOCKOUT_MS)) {
                m_lastActionFireTime[idx] = now;
                if (cb) {
                    cb();
                    stateChanged = true;
                }
            }
        }
    };

    // 1. Handle Toggle Actions
    handleToggleAction(HotkeyAction::TogglePitch, callbacksCopy.onTogglePitch);
    handleToggleAction(HotkeyAction::ToggleZoom, callbacksCopy.onToggleZoom);
    handleToggleAction(HotkeyAction::ResetDefaults, callbacksCopy.onResetDefaults);
    handleToggleAction(HotkeyAction::Exit, callbacksCopy.onExit);

    // 2. Handle Continuous Adjustment Actions (Altitude, Zoom, Pitch)
    if (IsActionKeyDown(HotkeyAction::AltitudeUp)) {
        const float delta = altitudeStep * speedMultiplier;
        if (callbacksCopy.onAdjustAltitude) {
            callbacksCopy.onAdjustAltitude(delta);
            stateChanged = true;
        }
    }

    if (IsActionKeyDown(HotkeyAction::AltitudeDown)) {
        const float delta = -altitudeStep * speedMultiplier;
        if (callbacksCopy.onAdjustAltitude) {
            callbacksCopy.onAdjustAltitude(delta);
            stateChanged = true;
        }
    }

    if (IsActionKeyDown(HotkeyAction::ZoomIn)) {
        const float delta = -zoomStep * speedMultiplier;
        if (callbacksCopy.onAdjustZoom) {
            callbacksCopy.onAdjustZoom(delta);
            stateChanged = true;
        }
    }

    if (IsActionKeyDown(HotkeyAction::ZoomOut)) {
        const float delta = zoomStep * speedMultiplier;
        if (callbacksCopy.onAdjustZoom) {
            callbacksCopy.onAdjustZoom(delta);
            stateChanged = true;
        }
    }

    if (IsActionKeyDown(HotkeyAction::PitchUp)) {
        const float delta = pitchStep * speedMultiplier;
        if (callbacksCopy.onAdjustPitch) {
            callbacksCopy.onAdjustPitch(delta);
            stateChanged = true;
        }
    }

    if (IsActionKeyDown(HotkeyAction::PitchDown)) {
        const float delta = -pitchStep * speedMultiplier;
        if (callbacksCopy.onAdjustPitch) {
            callbacksCopy.onAdjustPitch(delta);
            stateChanged = true;
        }
    }

    // Trigger state change observer if any action occurred
    if (stateChanged && callbacksCopy.onStateChanged) {
        callbacksCopy.onStateChanged();
    }
}

void HotkeyManager::BindPatcher(CameraPatcher* patcher) {
    if (!patcher) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_callbacks.onTogglePitch = [patcher]() {
        bool targetState = !patcher->IsPitchUnlocked();
        Logger::Instance().Log("HOTKEY", std::string("TogglePitch pressed -> setting unlock=") + (targetState ? "true" : "false"));
        patcher->SetPitchUnlocked(targetState);
    };

    m_callbacks.onToggleZoom = [patcher]() {
        bool targetState = !patcher->IsZoomUnlocked();
        Logger::Instance().Log("HOTKEY", std::string("ToggleZoom pressed -> setting unlock=") + (targetState ? "true" : "false"));
        patcher->SetZoomUnlocked(targetState);
    };

    m_callbacks.onAdjustAltitude = [patcher](float delta) {
        Logger::Instance().Log("HOTKEY", "AdjustAltitude triggered (delta=" + Logger::Float(delta) + ")");
        patcher->AdjustAltitude(delta);
    };

    m_callbacks.onAdjustZoom = [patcher](float delta) {
        Logger::Instance().Log("HOTKEY", "AdjustZoom triggered (delta=" + Logger::Float(delta) + ")");
        patcher->AdjustZoom(delta);
    };

    m_callbacks.onAdjustPitch = [patcher](float delta) {
        Logger::Instance().Log("HOTKEY", "AdjustPitch triggered (delta=" + Logger::Float(delta) + ")");
        patcher->AdjustPitch(delta);
    };

    m_callbacks.onResetDefaults = [patcher]() {
        Logger::Instance().Log("HOTKEY", "ResetDefaults pressed");
        patcher->ResetDefaults();
    };
}

void HotkeyManager::SetCallbacks(const HotkeyCallbacks& callbacks) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks = callbacks;
}

void HotkeyManager::SetOnTogglePitch(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onTogglePitch = std::move(cb);
}

void HotkeyManager::SetOnToggleZoom(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onToggleZoom = std::move(cb);
}

void HotkeyManager::SetOnAdjustAltitude(std::function<void(float delta)> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onAdjustAltitude = std::move(cb);
}

void HotkeyManager::SetOnAdjustZoom(std::function<void(float delta)> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onAdjustZoom = std::move(cb);
}

void HotkeyManager::SetOnAdjustPitch(std::function<void(float delta)> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onAdjustPitch = std::move(cb);
}

void HotkeyManager::SetOnResetDefaults(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onResetDefaults = std::move(cb);
}

void HotkeyManager::SetOnExit(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onExit = std::move(cb);
}

void HotkeyManager::SetOnStateChanged(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks.onStateChanged = std::move(cb);
}

void HotkeyManager::SetAltitudeStep(float step) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_altitudeStep = step;
}

float HotkeyManager::GetAltitudeStep() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_altitudeStep;
}

void HotkeyManager::SetZoomStep(float step) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_zoomStep = step;
}

float HotkeyManager::GetZoomStep() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_zoomStep;
}

void HotkeyManager::SetPitchStep(float step) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pitchStep = step;
}

float HotkeyManager::GetPitchStep() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pitchStep;
}

float HotkeyManager::CalculateSpeedMultiplier() const {
    const bool shiftDown = IsKeyDown(VK_SHIFT);
    const bool ctrlDown = IsKeyDown(VK_CONTROL);

    if (shiftDown && ctrlDown) {
        return MODIFIER_SPEED_BOOST * MODIFIER_SPEED_PRECISION; // 3.0f * 0.1f = 0.3f
    }
    if (shiftDown) {
        return MODIFIER_SPEED_BOOST; // 3.0f
    }
    if (ctrlDown) {
        return MODIFIER_SPEED_PRECISION; // 0.1f
    }
    return MODIFIER_SPEED_NORMAL; // 1.0f
}

bool HotkeyManager::IsKeyDown(int vKey) const {
    if (vKey == 0) {
        return false;
    }
    KeyStateProvider provider;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        provider = m_keyStateProvider;
    }
    if (provider) {
        return provider(vKey);
    }
    return (GetAsyncKeyState(vKey) & 0x8000) != 0;
}

bool HotkeyManager::IsActionKeyDown(HotkeyAction action) const {
    const size_t idx = static_cast<size_t>(action);
    if (idx >= m_bindings.size()) {
        return false;
    }
    KeyBinding binding{ 0, 0 };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        binding = m_bindings[idx];
    }
    return IsKeyDown(binding.primaryKey) || (binding.secondaryKey != 0 && IsKeyDown(binding.secondaryKey));
}

void HotkeyManager::SetKeyStateProvider(KeyStateProvider provider) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_keyStateProvider = std::move(provider);
}

void HotkeyManager::ResetKeyStateProvider() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_keyStateProvider = [](int vKey) -> bool {
        return (GetAsyncKeyState(vKey) & 0x8000) != 0;
    };
}

} // namespace ColdSteel3
