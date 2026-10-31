// gui/core/Event.h — pointer events routed through the widget tree.
#pragma once

#include "core/Types.h"

namespace immune::gui {

class Widget;

enum class PointerButton : u8 { Left = 0, Right = 1, Middle = 2 };

enum class EventType : u8 {
    Enter,      ///< Pointer moved onto the widget (not bubbled).
    Leave,      ///< Pointer moved off it (not bubbled).
    Down,
    Up,
    Click,      ///< Down and up on the same enabled widget.
    Deny,       ///< Down and up on the same DISABLED widget.
    DragStart,  ///< Pressed and moved past the drag threshold.
    Drag,       ///< `delta` since the last drag event.
    DragEnd,
    Wheel,
};

/// Feedback sounds the pointer state machine asks for. The gui knows nothing
/// about audio: it queues these and app/ maps them onto real sounds.
enum class UiSound : u8 {
    Hover,  ///< The pointer moved onto an enabled interactive widget.
    Click,  ///< Left click on an enabled widget.
    Deny,   ///< Left click on a disabled one.
};

struct Event {
    EventType type = EventType::Down;
    Vec2 pos{0.0f, 0.0f};    ///< Logical px.
    Vec2 delta{0.0f, 0.0f};  ///< Drag movement.
    f32 wheel = 0.0f;
    PointerButton button = PointerButton::Left;
    Widget* target = nullptr;
    /// Set by a handler to stop bubbling to ancestors.
    bool handled = false;
};

} // namespace immune::gui
