#pragma once

#include "camera_patcher.hpp"
#include <windows.h>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <array>
#include <string>

namespace ColdSteel3 {

// Timing and polling configuration constants
inline constexpr uint32_t HOTKEY_POLL_INTERVAL_MS = 16;     // ~60 Hz polling frequency
inline constexpr uint32_t TOGGLE_DEBOUNCE_LOCKOUT_MS = 200; // 200 ms lockout to prevent key flutter

// Speed modifier multiplier constants
inline constexpr float MODIFIER_SPEED_BOOST = 3.0f;     // VK_SHIFT: 3.0x speed boost
inline constexpr float MODIFIER_SPEED_PRECISION = 0.1f; // VK_CONTROL: 0.1x micro precision
inline constexpr float MODIFIER_SPEED_NORMAL = 1.0f;    // Unmodified: 1.0x baseline

// Base step increments
inline constexpr float DEFAULT_ALTITUDE_STEP = 0.25f; // Altitude units per tick
inline constexpr float DEFAULT_ZOOM_STEP = 0.50f;     // Zoom units per tick
inline constexpr float DEFAULT_PITCH_STEP = 0.50f;    // Pitch angle units per tick

/**
 * Enumeration of distinct hotkey actions supported by the utility.
 */
enum class HotkeyAction : size_t {
    TogglePitch = 0, // Toggle pitch clamp uncap
    ToggleZoom,      // Toggle zoom max distance uncap
    AltitudeUp,      // Raise camera height (continuous)
    AltitudeDown,    // Lower camera height (continuous)
    ZoomIn,          // Zoom in dynamically (continuous)
    ZoomOut,         // Zoom out dynamically (continuous)
    PitchUp,         // Pitch angle up dynamically (continuous)
    PitchDown,       // Pitch angle down dynamically (continuous)
    ResetDefaults,   // Reset all camera tweaks to game defaults
    Exit,            // Cleanly detach and exit utility
    ActionCount      // Total number of actions
};

/**
 * Dual key binding representation (primary and optional secondary fallback key).
 */
struct KeyBinding {
    int primaryKey = 0;
    int secondaryKey = 0;
};

/**
 * Observer callback bundle for hotkey events.
 */
struct HotkeyCallbacks {
    std::function<void()> onTogglePitch;
    std::function<void()> onToggleZoom;
    std::function<void(float delta)> onAdjustAltitude;
    std::function<void(float delta)> onAdjustZoom;
    std::function<void(float delta)> onAdjustPitch;
    std::function<void()> onResetDefaults;
    std::function<void()> onExit;
    std::function<void()> onStateChanged; // Notification for HUD update on state changes
};

/**
 * HotkeyManager
 *
 * High-performance, edge-triggered, debounced hotkey polling engine.
 * Runs at 60 Hz (Sleep(16)) and maps virtual key combinations to camera manipulation
 * callbacks or directly to CameraPatcher.
 */
class HotkeyManager {
public:
    using KeyStateProvider = std::function<bool(int vKey)>;
    using TimePoint = std::chrono::steady_clock::time_point;

    HotkeyManager();
    ~HotkeyManager();

    // Non-copyable and non-movable for RAII thread safety
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;
    HotkeyManager(HotkeyManager&&) = delete;
    HotkeyManager& operator=(HotkeyManager&&) = delete;

    /**
     * Start background 60 Hz polling thread.
     */
    void Start();

    /**
     * Stop background polling thread and join.
     */
    void Stop();

    /**
     * Check if background polling thread is active.
     */
    bool IsRunning() const;

    /**
     * Perform a single tick update.
     * Can be invoked manually in unit tests or single-threaded loops.
     */
    void Update();

    /**
     * Perform a single tick update with an explicit simulated timestamp.
     * Allows deterministic timing and debouncing verification in test harnesses.
     */
    void Update(TimePoint currentTime);

    /**
     * Connect key events directly to a CameraPatcher instance.
     * Automatically hooks Pitch, Zoom, Altitude, and Reset callbacks.
     */
    void BindPatcher(CameraPatcher* patcher);

    // Callback setters
    void SetCallbacks(const HotkeyCallbacks& callbacks);
    void SetOnTogglePitch(std::function<void()> cb);
    void SetOnToggleZoom(std::function<void()> cb);
    void SetOnAdjustAltitude(std::function<void(float delta)> cb);
    void SetOnAdjustZoom(std::function<void(float delta)> cb);
    void SetOnAdjustPitch(std::function<void(float delta)> cb);
    void SetOnResetDefaults(std::function<void()> cb);
    void SetOnExit(std::function<void()> cb);
    void SetOnStateChanged(std::function<void()> cb);

    // Step configuration
    void SetAltitudeStep(float step);
    float GetAltitudeStep() const;
    void SetZoomStep(float step);
    float GetZoomStep() const;
    void SetPitchStep(float step);
    float GetPitchStep() const;

    // Key binding configuration
    void SetBinding(HotkeyAction action, int primaryKey, int secondaryKey = 0);
    KeyBinding GetBinding(HotkeyAction action) const;
    void ResetDefaultBindings();

    // Speed modifier calculation
    float CalculateSpeedMultiplier() const;

    /**
     * Key state provider hook for deterministic unit testing.
     * If set, HotkeyManager queries this function instead of Win32 GetAsyncKeyState.
     */
    void SetKeyStateProvider(KeyStateProvider provider);
    void ResetKeyStateProvider();

private:
    // Internal polling loop
    void PollingThreadLoop();

    // Query physical or mocked key state
    bool IsKeyDown(int vKey) const;

    // Check if an action's assigned key(s) are pressed
    bool IsActionKeyDown(HotkeyAction action) const;

    // Mutex for thread safety
    mutable std::mutex m_mutex;

    // Thread management
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // Configured key bindings per action
    std::array<KeyBinding, static_cast<size_t>(HotkeyAction::ActionCount)> m_bindings;

    // Callbacks
    HotkeyCallbacks m_callbacks;

    // Base step increments
    float m_altitudeStep = DEFAULT_ALTITUDE_STEP;
    float m_zoomStep = DEFAULT_ZOOM_STEP;
    float m_pitchStep = DEFAULT_PITCH_STEP;

    // Edge-trigger and debounce tracking
    std::array<bool, static_cast<size_t>(HotkeyAction::ActionCount)> m_prevActionDown{};
    std::array<TimePoint, static_cast<size_t>(HotkeyAction::ActionCount)> m_lastActionFireTime{};

    // Key state provider (defaults to Win32 GetAsyncKeyState)
    KeyStateProvider m_keyStateProvider;
};

} // namespace ColdSteel3
