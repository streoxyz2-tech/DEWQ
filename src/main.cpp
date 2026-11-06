#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

using namespace geode::prelude;

namespace {

constexpr int kJumpButton = static_cast<int>(PlayerButton::Jump);
constexpr int kHalfStepsPerTick = 2;

bool g_bufferEnabled = true;
bool g_bufferReleases = true;
bool g_debugToasts = false;
int64_t g_bufferTicks = 4;

bool g_replayingInput = false;
GJBaseGameLayer* g_activeLayer = nullptr;

struct PlayerBuffer {
    bool waitingPress = false;
    int pressStep = 0;

    bool waitingRelease = false;
    int releaseStep = 0;

    bool scheduledRelease = false;
    int releaseDueStep = 0;

    bool hasForwardedPress = false;
    int forwardedPressStep = 0;
};

std::array<PlayerBuffer, 2> g_players{};

void clearBuffers() {
    g_players = {};
}

void debugEvent(std::string const& message) {
    if (!g_debugToasts) {
        return;
    }

    log::info("[Predictive Input Buffer] {}", message);
    Notification::create(message, NotificationIcon::None, 1.f)->show();
}

void setBufferEnabled(bool enabled) {
    g_bufferEnabled = enabled;
    Mod::get()->setSettingValue<bool>("buffer-enabled", enabled);

    if (!enabled) {
        clearBuffers();
    }

    Notification::create(
        enabled ? "Predictive Input Buffer: ON" : "Predictive Input Buffer: OFF",
        enabled ? NotificationIcon::Success : NotificationIcon::None,
        1.f
    )->show();
}

void toggleBuffer() {
    setBufferEnabled(!g_bufferEnabled);
}

PlayerBuffer& stateFor(bool isPlayer1) {
    return g_players[isPlayer1 ? 0 : 1];
}

bool isAlwaysResponsiveMode(PlayerObject* player) {
    return player->m_isShip ||
        player->m_isBird ||
        player->m_isDart ||
        player->m_isSwing;
}

// Uses the requested vanilla-state categories without calling pushButton
// speculatively (pushButton mutates player state).
bool canJumpOnCurrentStep(PlayerObject* player) {
    if (!player || player->m_isDead) {
        return false;
    }

    if (isAlwaysResponsiveMode(player)) {
        return true;
    }

    return player->m_isOnGround ||
        player->m_isOnSlope ||
        (player->m_touchingRings && player->m_touchingRings->count() > 0);
}

int elapsedTicks(int currentStep, int pressStep) {
    int const halfSteps = std::max(0, currentStep - pressStep);
    return (halfSteps + kHalfStepsPerTick - 1) / kHalfStepsPerTick;
}

} // namespace

class $modify(PredictiveInputBufferLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Replayed inputs re-enter this hook once, then go directly to vanilla.
        if (g_replayingInput) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (button != kJumpButton || !g_bufferEnabled || this->m_playerDied) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        PlayerObject* player = isPlayer1 ? this->m_player1 : this->m_player2;
        if (!player || player->m_isDead) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& state = stateFor(isPlayer1);
        int const step = this->m_currentStep;

        if (down) {
            // Do not stack multiple waiting presses for one player.
            if (state.waitingPress) {
                return;
            }

            if (canJumpOnCurrentStep(player)) {
                state.hasForwardedPress = true;
                state.forwardedPressStep = step;
                GJBaseGameLayer::handleButton(down, button, isPlayer1);
                debugEvent("passed through (valid)");
                return;
            }

            state.waitingPress = true;
            state.pressStep = step;
            state.waitingRelease = false;
            state.scheduledRelease = false;

            debugEvent("buffered (waiting)");
            return;
        }

        // Releases are not predicted. They are only held to preserve the
        // original press/release pair when that press is still waiting.
        if (!g_bufferReleases) {
            // If the press never reached the game, do not replay it after its
            // physical release; that would create a stuck synthetic hold.
            if (state.waitingPress) {
                state.waitingPress = false;
                state.waitingRelease = false;
            }

            state.hasForwardedPress = false;
            state.scheduledRelease = false;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (state.waitingPress) {
            if (!state.waitingRelease) {
                state.waitingRelease = true;
                state.releaseStep = step;
            }
            return;
        }

        // If the release is in the same physics step as its press, defer it
        // one full tick so vanilla has a chance to register the press.
        if (state.hasForwardedPress && state.forwardedPressStep == step) {
            state.scheduledRelease = true;
            state.releaseDueStep = step + kHalfStepsPerTick;
            return;
        }

        state.hasForwardedPress = false;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // In 2.2081, processCommands advances m_currentStep, consumes queued
        // inputs (through processQueuedButtons -> handleButton), and returns
        // immediately before the player update calls in the physics loop.
        // The isHalfTick flag distinguishes subdivisions from full ticks.
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (g_activeLayer != this) {
            clearBuffers();
            g_activeLayer = this;
        }

        // Run once per full physics tick, not once per half-tick or rendered
        // frame. This is after timestamped queue processing and before the
        // corresponding PlayerObject::update.
        if (isHalfTick) {
            return;
        }

        if (this->m_playerDied ||
            (this->m_player1 && this->m_player1->m_isDead) ||
            (this->m_player2 && this->m_player2->m_isDead)) {
            clearBuffers();
            return;
        }

        int const step = this->m_currentStep;
        processPlayer(0, this->m_player1, true, step);
        processPlayer(1, this->m_player2, false, step);
    }

    void processPlayer(
        std::size_t index,
        PlayerObject* player,
        bool isPlayer1,
        int currentStep
    ) {
        if (!player) {
            g_players[index] = {};
            return;
        }

        auto& state = g_players[index];

        if (state.waitingPress) {
            int const age = elapsedTicks(currentStep, state.pressStep);

            // Expiry is checked before validity, so a press more than N ticks
            // early cannot be rescued on tick N+1.
            if (age > g_bufferTicks) {
                state.waitingPress = false;
                state.waitingRelease = false;
                debugEvent("dropped (too early)");
            } else if (canJumpOnCurrentStep(player)) {
                state.waitingPress = false;
                replayJump(true, isPlayer1);

                state.hasForwardedPress = true;
                state.forwardedPressStep = currentStep;

                debugEvent("fired after " + std::to_string(age) + " ticks");

                if (state.waitingRelease) {
                    int const requestedHold = std::max(
                        kHalfStepsPerTick,
                        state.releaseStep - state.pressStep
                    );

                    int due = currentStep + requestedHold;
                    if ((due & 1) != 0) {
                        ++due;
                    }

                    state.scheduledRelease = true;
                    state.releaseDueStep = due;
                    state.waitingRelease = false;
                }
            }
        }

        if (state.scheduledRelease && currentStep >= state.releaseDueStep) {
            replayJump(false, isPlayer1);
            state.scheduledRelease = false;
            state.hasForwardedPress = false;
        }
    }

    void replayJump(bool down, bool isPlayer1) {
        g_replayingInput = true;
        this->handleButton(down, kJumpButton, isPlayer1);
        g_replayingInput = false;
    }
};

class $modify(PredictiveInputBufferPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        clearBuffers();
        g_activeLayer = nullptr;
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void pauseGame(bool unfocused) {
        clearBuffers();
        PlayLayer::pauseGame(unfocused);
    }

    void resetLevel() {
        clearBuffers();
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        clearBuffers();
        PlayLayer::resetLevelFromStart();
    }

    void fullReset() {
        clearBuffers();
        PlayLayer::fullReset();
    }

    void resumeAndRestart(bool fromStart) {
        clearBuffers();
        PlayLayer::resumeAndRestart(fromStart);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        clearBuffers();
        PlayLayer::destroyPlayer(player, object);
    }

    void onExit() {
        clearBuffers();
        g_activeLayer = nullptr;
        PlayLayer::onExit();
    }

    void onQuit() {
        clearBuffers();
        g_activeLayer = nullptr;
        PlayLayer::onQuit();
    }
};

$on_mod(Loaded) {
    g_bufferEnabled = Mod::get()->getSettingValue<bool>("buffer-enabled");
    g_bufferTicks = Mod::get()->getSettingValue<int64_t>("buffer-ticks");
    g_bufferReleases = Mod::get()->getSettingValue<bool>("buffer-releases");
    g_debugToasts = Mod::get()->getSettingValue<bool>("debug-toasts");

    listenForSettingChanges<bool>("buffer-enabled", [](bool value) {
        g_bufferEnabled = value;
        if (!value) {
            clearBuffers();
        }
    });

    listenForSettingChanges<int64_t>("buffer-ticks", [](int64_t value) {
        g_bufferTicks = std::clamp<int64_t>(value, 2, 16);
    });

    listenForSettingChanges<bool>("buffer-releases", [](bool value) {
        g_bufferReleases = value;
    });

    listenForSettingChanges<bool>("debug-toasts", [](bool value) {
        g_debugToasts = value;
    });
}

$on_game(Loaded) {
    listenForKeybindSettingPresses(
        "toggle-keybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (down && !repeat) {
                toggleBuffer();
            }
        }
    );
}
