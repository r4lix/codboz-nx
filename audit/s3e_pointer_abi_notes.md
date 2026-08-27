# s3ePointer ABI notes for Claude

Purpose: provide a concrete implementation/review target for the synthetic-tap test in the Black Ops Zombies Marmalade/Airplay port.

This file is documentation only. Codex did not modify the interpreter, HLE implementation, or Switch source files.

## Working enum values

The classic Airplay/Marmalade `s3ePointer.h` ABI uses these values:

```c
typedef enum s3ePointerState
{
    S3E_POINTER_STATE_UP       = 0,
    S3E_POINTER_STATE_DOWN     = 1,
    S3E_POINTER_STATE_PRESSED  = 2,
    S3E_POINTER_STATE_RELEASED = 3,
    S3E_POINTER_STATE_UNKNOWN  = 4
} s3ePointerState;
```

```c
typedef enum s3ePointerProperty
{
    S3E_POINTER_AVAILABLE             = 0,
    S3E_POINTER_HIDE_CURSOR           = 1,
    S3E_POINTER_TYPE                  = 2,
    S3E_POINTER_STYLUS_TYPE           = 3,
    S3E_POINTER_MULTI_TOUCH_AVAILABLE = 4
} s3ePointerProperty;
```

Associated property return values:

```c
typedef enum s3ePointerType
{
    S3E_POINTER_TYPE_INVALID = 0,
    S3E_POINTER_TYPE_MOUSE   = 1,
    S3E_POINTER_TYPE_STYLUS  = 2
} s3ePointerType;

typedef enum s3eStylusType
{
    S3E_STYLUS_TYPE_INVALID = 0,
    S3E_STYLUS_TYPE_STYLUS  = 1,
    S3E_STYLUS_TYPE_FINGER  = 2
} s3eStylusType;
```

Buttons:

```c
typedef enum s3ePointerButton
{
    S3E_POINTER_BUTTON_SELECT         = 0,
    S3E_POINTER_BUTTON_LEFTMOUSE      = 0, /* alias */
    S3E_POINTER_BUTTON_RIGHTMOUSE     = 1,
    S3E_POINTER_BUTTON_MIDDLEMOUSE    = 2,
    S3E_POINTER_BUTTON_MOUSEWHEELUP   = 3,
    S3E_POINTER_BUTTON_MOUSEWHEELDOWN = 4
} s3ePointerButton;
```

## Guest ARM callback layouts

These are 32-bit guest layouts. Do not use the size of an equivalent host structure if host ABI packing differs.

```c
typedef struct s3ePointerEvent
{
    s3ePointerButton m_Button;  /* +0x00, 4 bytes */
    uint32           m_Pressed; /* +0x04, 4 bytes: 1 press, 0 release */
    int32            m_x;       /* +0x08, 4 bytes */
    int32            m_y;       /* +0x0C, 4 bytes */
} s3ePointerEvent;              /* size 16, alignment 4 */

typedef struct s3ePointerMotionEvent
{
    int32 m_x;                  /* +0x00, 4 bytes */
    int32 m_y;                  /* +0x04, 4 bytes */
} s3ePointerMotionEvent;        /* size 8, alignment 4 */
```

Some published header copies describe `m_Pressed` as `int32` rather than `uint32`; this does not change the 32-bit ABI or offsets.

## Update behavior

The function signature is:

```c
void s3ePointerUpdate(void);
```

It is not an `s3eResult` function. It updates the SDK's cached pointer state and pumps queued pointer callbacks. The normal SDK contract is to call it once per frame before polling `s3ePointerGetState`, `s3ePointerGetX`, or `s3ePointerGetY` for fresh input.

For a deterministic synthetic tap, expose this state sequence on consecutive `s3ePointerUpdate()` calls:

```text
PRESSED (2) -> DOWN (1) -> RELEASED (3) -> UP (0)
```

Suggested single-touch capability responses for the first test:

```text
S3E_POINTER_AVAILABLE             -> 1
S3E_POINTER_MULTI_TOUCH_AVAILABLE -> 0
S3E_POINTER_TYPE                  -> 2  (STYLUS/touchscreen)
S3E_POINTER_STYLUS_TYPE           -> 2  (FINGER)
```

Button callback payloads:

```text
press:   { m_Button=0, m_Pressed=1, m_x=x, m_y=y }
release: { m_Button=0, m_Pressed=0, m_x=x, m_y=y }
```

The coordinates should remain at the last known tap location after release.

## Review checklist

1. Check these numeric values against the exact Airplay/Marmalade SDK revision inferred from the S3E binary, if an original header is available.
2. Confirm the game calls `s3ePointerUpdate`; do not advance transient states from `GetState` calls.
3. Confirm callback data is written as guest little-endian 32-bit words at offsets 0, 4, 8, and 12.
4. Confirm both paths see the same tap: the registered button callback and polling through `GetState/GetX/GetY`.
5. Log the first frame/update where each state is visible, plus whether callback dispatch occurred.
6. Compare frame hashes, distinct-colour counts, and control flow before and after injection.
7. If the game does not react, keep the capability responses logged before changing them; the next target is the `s3eSurfaceSetup`/`s3eGLGetInt`/`s3eConfigGetInt` gate audit.

## Public corroborating sources

- Dear ImGui's Marmalade backend consumes `s3ePointerEvent` fields and polls `s3ePointerGetState`: https://skia.googlesource.com/external/github.com/ocornut/imgui/+/8e48ab6b19d153c2ff9b56379ff1fb60034ca133/examples/marmalade_example/imgui_impl_marmalade.cpp
- Marmalade pointer tutorial showing callback payload use, multi-touch event fields, and per-frame `s3ePointerUpdate`: https://www.drmop.com/index.php/2011/09/24/marmalade-sdk-tutorial-touch-and-multi-touch/

Important: the original proprietary vendor header was not publicly accessible during this audit. Treat the numeric table as the concrete working target, but perform checklist item 1 if Claude has access to an SDK header matching this game's build era.
