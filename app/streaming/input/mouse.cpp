#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"
#include "streaming/streamutils.h"

void SdlInputHandler::handleMouseButtonEvent(SDL_MouseButtonEvent* event)
{
    int button;

    if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }
    else if (!isCaptureActive()) {
        if (event->button == SDL_BUTTON_LEFT && event->state == SDL_RELEASED &&
                isMouseInVideoRegion(event->x, event->y)) {
            // Capture the mouse again if clicked when unbound.
            // We start capture on left button released instead of
            // pressed to avoid sending an errant mouse button released
            // event to the host when clicking into our window (since
            // the pressed event was consumed by this code).
            setCaptureActive(true);
        }

        // Not capturing
        return;
    }
    else if (m_AbsoluteMouseMode && !isMouseInVideoRegion(event->x, event->y) && event->state == SDL_PRESSED) {
        // Ignore button presses outside the video region, but allow button releases
        return;
    }

    switch (event->button)
    {
        case SDL_BUTTON_LEFT:
            button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            button = BUTTON_RIGHT;
            break;
        case SDL_BUTTON_X1:
            button = BUTTON_X1;
            break;
        case SDL_BUTTON_X2:
            button = BUTTON_X2;
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unhandled button event: %d",
                        event->button);
            return;
    }

    if (m_SwapMouseButtons) {
        if (button == BUTTON_RIGHT)
            button = BUTTON_LEFT;
        else if (button == BUTTON_LEFT)
            button = BUTTON_RIGHT;
    }

    LiSendMouseButtonEvent(event->state == SDL_PRESSED ?
                               BUTTON_ACTION_PRESS :
                               BUTTON_ACTION_RELEASE,
                           button);
}

void SdlInputHandler::handleMouseMotionEvent(SDL_MouseMotionEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    // Capture the source window from the event before batching
    Uint32 sourceWindowID = event->windowID;

    // Batch all pending mouse motion events from the same window
    Sint32 x = event->x, y = event->y, xrel = event->xrel, yrel = event->yrel;
    SDL_Event nextEvent;
    while (SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION) > 0) {
        if (nextEvent.motion.which == SDL_TOUCH_MOUSEID) {
            continue;
        }
        if (m_MultiMonitorEnabled && nextEvent.motion.windowID != sourceWindowID) {
            // Put back events from a different window
            SDL_PeepEvents(&nextEvent, 1, SDL_ADDEVENT, 0, 0);
            break;
        }
        x = nextEvent.motion.x;
        y = nextEvent.motion.y;
        xrel += nextEvent.motion.xrel;
        yrel += nextEvent.motion.yrel;
    }

    // We should not reference the original event anymore
    event = nullptr;

    if (m_AbsoluteMouseMode) {
        // Resolve which visible window this event came from
        SDL_Window* activeWin = getWindowForEvent(sourceWindowID);
        int windowWidth, windowHeight;
        SDL_GetWindowSize(activeWin, &windowWidth, &windowHeight);

        // Multi-monitor: compute X offset based on which monitor window this is
        int multiMonitorXOffset = 0;
        if (m_MultiMonitorEnabled && m_MultiMonitorCount > 1) {
            multiMonitorXOffset = getMonitorIndex(activeWin) * m_PerMonitorWidth;
        }

        SDL_Rect src, dst;
        bool mouseInVideoRegion;

        // In multi-monitor mode, each window shows one monitor's slice
        src.x = src.y = 0;
        if (m_MultiMonitorEnabled && m_MultiMonitorCount > 1) {
            src.w = m_PerMonitorWidth;
            src.h = m_PerMonitorHeight;
        } else {
            src.w = m_StreamWidth;
            src.h = m_StreamHeight;
        }

        dst.x = dst.y = 0;
        dst.w = windowWidth;
        dst.h = windowHeight;

        // Use the stream and window sizes to determine the video region
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        mouseInVideoRegion = isMouseInVideoRegion(x, y, windowWidth, windowHeight);

        // Clamp motion to the video region
        x = qMin(qMax(x - dst.x, 0), dst.w);
        y = qMin(qMax(y - dst.y, 0), dst.h);

        // Send the mouse position update if one of the following is true:
        // a) it is in the video region now
        // b) it just left the video region (to ensure the mouse is clamped to the video boundary)
        // c) a mouse button is still down from before the cursor left the video region (to allow smooth dragging)
        Uint32 buttonState = SDL_GetMouseState(nullptr, nullptr);
        if (buttonState == 0) {
            if (m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
                // Stop capturing the mouse now
                SDL_CaptureMouse(SDL_FALSE);
                m_PendingMouseButtonsAllUpOnVideoRegionLeave = false;
            }
        }
        if (mouseInVideoRegion || m_MouseWasInVideoRegion || m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
            if (m_MultiMonitorEnabled && m_MultiMonitorCount > 1) {
                // In multi-monitor mode, send coordinates in the combined virtual desktop space
                LiSendMousePositionEvent((short)(x + multiMonitorXOffset), (short)y, m_StreamWidth, m_StreamHeight);
            } else {
                LiSendMousePositionEvent((short)x, (short)y, dst.w, dst.h);
            }
        }

        // Adjust the cursor visibility if applicable
        if (mouseInVideoRegion ^ m_MouseWasInVideoRegion) {
            SDL_ShowCursor((mouseInVideoRegion && m_MouseCursorCapturedVisibilityState == SDL_DISABLE) ? SDL_DISABLE : SDL_ENABLE);
            if (!mouseInVideoRegion && buttonState != 0) {
                m_PendingMouseButtonsAllUpOnVideoRegionLeave = true;
            }
        }

        m_MouseWasInVideoRegion = mouseInVideoRegion;
    }
    else {
        LiSendMouseMoveEvent(xrel, yrel);
    }
}

void SdlInputHandler::handleMouseWheelEvent(SDL_MouseWheelEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    if (m_AbsoluteMouseMode) {
        int mouseX, mouseY;
        SDL_GetMouseState(&mouseX, &mouseY);
        if (!isMouseInVideoRegion(mouseX, mouseY)) {
            // Ignore scroll events outside the video region
            return;
        }
    }

#if SDL_VERSION_ATLEAST(2, 0, 18)
    if (event->preciseY != 0.0f) {
        // Invert the scroll direction if needed
        if (m_ReverseScrollDirection) {
            event->preciseY = -event->preciseY;
        }

#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->preciseY = SDL_clamp(event->preciseY, -1.0f, 1.0f);
#endif

        LiSendHighResScrollEvent((short)(event->preciseY * 120)); // WHEEL_DELTA
    }

    if (event->preciseX != 0.0f) {
        // Invert the scroll direction if needed
        if (m_ReverseScrollDirection) {
            event->preciseX = -event->preciseY;
        }

#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->preciseX = SDL_clamp(event->preciseX, -1.0f, 1.0f);
#endif

        LiSendHighResHScrollEvent((short)(event->preciseX * 120)); // WHEEL_DELTA
    }
#else
    if (event->y != 0) {
        // Invert the scroll direction if needed
        if (m_ReverseScrollDirection) {
            event->y = -event->y;
        }

#ifdef Q_OS_DARWIN
        // See comment above
        event->y = SDL_clamp(event->y, -1, 1);
#endif

        LiSendScrollEvent((signed char)event->y);
    }

    if (event->x != 0) {
        // Invert the scroll direction if needed
        if (m_ReverseScrollDirection) {
            event->x = -event->x;
        }

#ifdef Q_OS_DARWIN
        // See comment above
        event->x = SDL_clamp(event->x, -1, 1);
#endif

        LiSendHScrollEvent((signed char)event->x);
    }
#endif
}

bool SdlInputHandler::isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth, int windowHeight)
{
    SDL_Rect src, dst;

    if (windowWidth < 0 || windowHeight < 0) {
        SDL_GetWindowSize(getActiveWindow(), &windowWidth, &windowHeight);
    }

    src.x = src.y = 0;
    if (m_MultiMonitorEnabled && m_MultiMonitorCount > 1) {
        src.w = m_PerMonitorWidth;
        src.h = m_PerMonitorHeight;
    } else {
        src.w = m_StreamWidth;
        src.h = m_StreamHeight;
    }

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Use the stream and window sizes to determine the video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    return (mouseX >= dst.x && mouseX <= dst.x + dst.w) &&
           (mouseY >= dst.y && mouseY <= dst.y + dst.h);
}

void SdlInputHandler::updatePointerRegionLock()
{
    // Pointer region lock is irrelevant in relative mouse mode
    if (SDL_GetRelativeMouseMode()) {
        return;
    }

    // Our pointer lock behavior tracks with the fullscreen mode unless the user has
    // toggled it themselves using the keyboard shortcut. If that's the case, they
    // have full control over it and we don't touch it anymore.
    if (!m_PointerRegionLockToggledByUser) {
        Uint32 fullscreenFlags = SDL_GetWindowFlags(getActiveWindow()) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        m_PointerRegionLockActive = (fullscreenFlags == SDL_WINDOW_FULLSCREEN) ||
                                    (fullscreenFlags != 0 && SDL_GetNumVideoDisplays() == 1);
    }

    // Helper to apply or release pointer lock on a single window
    auto applyLock = [this](SDL_Window* win, bool lock) {
        if (lock) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_Rect src, dst;
            src.x = src.y = 0;
            if (m_MultiMonitorEnabled && m_MultiMonitorCount > 1) {
                src.w = m_PerMonitorWidth;
                src.h = m_PerMonitorHeight;
            } else {
                src.w = m_StreamWidth;
                src.h = m_StreamHeight;
            }
            dst.x = dst.y = 0;
            SDL_GetWindowSize(win, &dst.w, &dst.h);
            StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
            SDL_SetWindowMouseRect(win, &dst);
#elif SDL_VERSION_ATLEAST(2, 0, 15)
            SDL_SetWindowMouseGrab(win, SDL_TRUE);
#else
            SDL_SetWindowGrab(win, SDL_TRUE);
#endif
        } else {
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_SetWindowMouseRect(win, nullptr);
#elif SDL_VERSION_ATLEAST(2, 0, 15)
            SDL_SetWindowMouseGrab(win, SDL_FALSE);
#else
            SDL_SetWindowGrab(win, SDL_FALSE);
#endif
        }
    };

    bool lock = isCaptureActive() && m_PointerRegionLockActive;
    if (m_MultiMonitorEnabled) {
        for (auto* win : m_MultiMonitorWindows) {
            if (win) applyLock(win, lock);
        }
    } else {
        applyLock(m_Window, lock);
    }
}
