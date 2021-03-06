/*
 * Copyright 2007 Peter Hutterer
 * Copyright 2009 Przemysław Firszt
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <linux/input.h>
#include <linux/types.h>

#include <xf86_OSproc.h>

#include <unistd.h>

#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>
#include <xorgVersion.h>
#include <xkbsrv.h>
#include <libevdev-1.0/libevdev/libevdev.h>

#ifdef HAVE_PROPERTIES
#include <xserver-properties.h>
/* 1.6 has properties, but no labels */
#ifdef AXIS_LABEL_PROP
#define HAVE_LABELS
#else
#undef HAVE_LABELS
#endif

#endif


#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <xorg-server.h>
#include <xorgVersion.h>
#include <xf86Module.h>
#include <X11/Xatom.h>

#include "random.h"


/* random_driver_name[] fixes a gcc warning:
 * "initialization discards 'const' qualifier from pointer target type" */
static char random_driver_name[] = "random";

_X_EXPORT InputDriverRec RANDOM = {
    1,
    random_driver_name,
    NULL,
    RandomPreInit,
    RandomUnInit,
    NULL,
    0
};

static XF86ModuleVersionInfo RandomVersionRec =
{
    "random",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData randomModuleData =
{
    &RandomVersionRec,
    &RandomPlug,
    &RandomUnplug
};

static void
RandomUnplug(pointer p)
{
};

static pointer
RandomPlug(pointer        module,
           pointer        options,
           int            *errmaj,
           int            *errmin)
{
    xf86AddInputDriver(&RANDOM, module, 0);
    return module;
};

static int RandomPreInit(InputDriverPtr  drv,
                         InputInfoPtr    pInfo,
                         int             flags)
{
    RandomDevicePtr    pRandom;
    int res;


    pRandom = calloc(1, sizeof(RandomDeviceRec));
    if (!pRandom) {
        pInfo->private = NULL;
        xf86DeleteInput(pInfo, 0);
        return BadAlloc;
    }

    pInfo->private = pRandom;
    pInfo->type_name = strdup(XI_MOUSE); /* see XI.h */
    pInfo->read_input = RandomReadInput; /* new data avl */
    pInfo->switch_mode = NULL; /* toggle absolute/relative mode */
    pInfo->device_control = RandomControl; /* enable/disable dev */
    /* process driver specific options */
    pRandom->device = xf86SetStrOption(pInfo->options,
                                       "Device",
                                       "/dev/input/event8");

    PRINT_INFO("%s: Using device %s.\n", pInfo->name, pRandom->device);

    /* process generic options */
    xf86CollectInputOptions(pInfo, NULL);
    xf86ProcessCommonOptions(pInfo, pInfo->options);
    /* Open sockets, init device files, etc. */
    SYSCALL(pInfo->fd = open(pRandom->device, O_RDWR | O_NONBLOCK));
    if (pInfo->fd == -1)
    {
        xf86Msg(X_ERROR, "%s: failed to open %s.",
                pInfo->name, pRandom->device);
        pInfo->private = NULL;
        free(pRandom);
        xf86DeleteInput(pInfo, 0);
        return BadAccess;
    }

    pRandom->evdev = libevdev_new();
    res = libevdev_set_fd(pRandom->evdev, pInfo->fd);
    if (res != 0) {
        xf86Msg(X_ERROR, "Cannot associate fd %i with libevdev\n", pInfo->fd);
        libevdev_free(pRandom->evdev);
        return BadAccess;
    }
    clear_state(&pRandom->state);

    // create a timer used for timeout during 3-finger-drag
    pRandom->state.timer = TimerSet(NULL, 0, 0, NULL, NULL);

    /* do more funky stuff */
    close(pInfo->fd);
    pInfo->fd = -1;
    return Success;
}

static void RandomUnInit(InputDriverPtr drv,
                         InputInfoPtr   pInfo,
                         int            flags)
{
    RandomDevicePtr     pRandom = pInfo->private;
    if (pRandom->device)
    {
        free(pRandom->device);
        pRandom->device = NULL;
    }
    if (pRandom && pRandom->state.timer) {
        free(pRandom->state.timer);
    }
    free(pInfo->private);
    /* Common error - pInfo->private must be NULL or valid memoy before
     * passing into xf86DeleteInput */
    pInfo->private = NULL;
    xf86DeleteInput(pInfo, 0);
}


static int
_random_init_buttons(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    RandomDevicePtr     pRandom = pInfo->private;
    CARD8               *map;
    int                 i;
    int                 ret = Success;
    const int           num_buttons = 2;

    map = calloc(num_buttons, sizeof(CARD8));

    for (i = 0; i < num_buttons; i++)
        map[i] = i;

    pRandom->labels = calloc(1, sizeof(Atom));

    if (!InitButtonClassDeviceStruct(device, num_buttons, pRandom->labels, map)) {
            xf86Msg(X_ERROR, "%s: Failed to register buttons.\n", pInfo->name);
            ret = BadAlloc;
    }

    free(map);
    return ret;
}

static void RandomInitAxesLabels(RandomDevicePtr pRandom, int natoms, Atom *atoms)
{
#ifdef HAVE_LABELS
    Atom atom;
    int axis;
    char **labels;
    int labels_len = 0;
    char *misc_label;

    labels     = rel_labels;
    labels_len = ArrayLength(rel_labels);
    misc_label = AXIS_LABEL_PROP_REL_MISC;

    memset(atoms, 0, natoms * sizeof(Atom));

    /* Now fill the ones we know */
    for (axis = 0; axis < labels_len; axis++)
    {
        if (pRandom->axis_map[axis] == -1)
            continue;

        atom = XIGetKnownProperty(labels[axis]);
        if (!atom) /* Should not happen */
            continue;

        atoms[pRandom->axis_map[axis]] = atom;
    }
#endif
}


static int
_random_init_axes(DeviceIntPtr device)
{
    InputInfoPtr        pInfo = device->public.devicePrivate;
    RandomDevicePtr     pRandom = pInfo->private;
    int                 i;
    const int           num_axes = 2;
    Atom                * atoms;

    pRandom->num_vals = num_axes;
    atoms = calloc(pRandom->num_vals, sizeof(Atom));

    RandomInitAxesLabels(pRandom, pRandom->num_vals, atoms);
    if (!InitValuatorClassDeviceStruct(device,
                num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                atoms,
#endif
                GetMotionHistorySize(),
                0))
        return BadAlloc;

    for (i = 0; i < pRandom->axes; i++) {
            xf86InitValuatorAxisStruct(device, i, *pRandom->labels,
                                       -1, -1, 1, 1, 1, Absolute);
            xf86InitValuatorDefaults(device, i);
    }
    free(atoms);
    return Success;
}

static int RandomControl(DeviceIntPtr    device,
                         int             what)
{
    InputInfoPtr  pInfo = device->public.devicePrivate;
    RandomDevicePtr pRandom = pInfo->private;

    switch(what)
    {
        case DEVICE_INIT:
            _random_init_buttons(device);
            _random_init_axes(device);
            break;

        /* Switch device on.  Establish socket, start event delivery.  */
        case DEVICE_ON:
            PRINT_INFO("%s: On.\n", pInfo->name);
            if (device->public.on)
                    break;

            SYSCALL(pInfo->fd = open(pRandom->device, O_RDONLY | O_NONBLOCK));
            if (pInfo->fd < 0)
            {
                xf86Msg(X_ERROR, "%s: cannot open device.\n", pInfo->name);
                return BadRequest;
            }

            xf86FlushInput(pInfo->fd);
            xf86AddEnabledDevice(pInfo);
            device->public.on = TRUE;
            break;
       case DEVICE_OFF:
            PRINT_INFO("%s: Off.\n", pInfo->name);
            if (!device->public.on)
                break;
            xf86RemoveEnabledDevice(pInfo);
            close(pInfo->fd);
            pInfo->fd = -1;
            device->public.on = FALSE;
            break;
      case DEVICE_CLOSE:
            /* free what we have to free */
            break;
    }
    return Success;
}

const char *type_and_code_name(int type, int code) {
    switch (type) {
        case EV_SYN:
        return "EV_SYN";
        break;
        case EV_KEY:
        switch (code) {
            case BTN_LEFT:
            return "EV_KEY BTN_LEFT";
            break;
            case BTN_TOOL_FINGER:
            return "EV_KEY BTN_TOOL_FINGER";
            break;
            case BTN_TOOL_QUINTTAP:
            return "EV_KEY BTN_TOOL_QUINTTAP";
            break;
            case BTN_TOUCH:
            return "EV_KEY BTN_TOUCH";
            break;
            case BTN_TOOL_DOUBLETAP:
            return "EV_KEY BTN_TOOL_DOUBLETAP";
            break;
            case BTN_TOOL_TRIPLETAP:
            return "EV_KEY BTN_TOOL_TRIPLETAP";
            break;
            case BTN_TOOL_QUADTAP:
            return "EV_KEY BTN_TOOL_QUADTAP";
            break;
        }
        break;
        case EV_ABS:
        switch (code) {
            case ABS_X:
            return "EV_ABS ABS_X";
            break;
            case ABS_Y:
            return "EV_ABS ABS_Y";
            break;
            case ABS_PRESSURE:
            return "EV_ABS ABS_PRESSURE";
            break;
            case ABS_TOOL_WIDTH:
            return "EV_ABS ABS_TOOL_WIDTH";
            break;
            case ABS_MT_SLOT:
            return "EV_ABS ABS_MT_SLOT";
            break;
            case ABS_MT_TOUCH_MAJOR:
            return "EV_ABS ABS_MT_TOUCH_MAJOR";
            break;
            case ABS_MT_TOUCH_MINOR:
            return "EV_ABS ABS_MT_TOUCH_MINOR";
            break;
            case ABS_MT_WIDTH_MAJOR:
            return "EV_ABS ABS_MT_WIDTH_MAJOR";
            break;
            case ABS_MT_WIDTH_MINOR:
            return "EV_ABS ABS_MT_WIDTH_MINOR";
            break;
            case ABS_MT_ORIENTATION:
            return "EV_ABS ABS_MT_ORIENTATION";
            break;
            case ABS_MT_POSITION_X:
            return "EV_ABS ABS_MT_POSITION_X";
            break;
            case ABS_MT_POSITION_Y:
            return "EV_ABS ABS_MT_POSITION_Y";
            break;
            case ABS_MT_TRACKING_ID:
            return "EV_ABS ABS_MT_TRACKING_ID";
            break;
        }
        break;
    }
    return "undefined";
}
const char *touchpad_state_name(enum TouchpadStates state) {
    switch (state) {
        case TS_DEFAULT:
        return "TS_DEFAULT";
        break;
        case TS_2_FINGER_SCROLL:
        return "TS_2_FINGER_SCROLL";
        break;
        case TS_2_FINGER_SCROLL_RELEASING:
        return "TS_2_FINGER_SCROLL_RELEASING";
        break;
        case TS_2_FINGER_SCROLL_MOMENTUM:
        return "TS_2_FINGER_SCROLL_MOMENTUM";
        break;
        case TS_3_FINGER_DRAG:
        return "TS_3_FINGER_DRAG";
        break;
        case TS_3_FINGER_DRAG_RELEASING:
        return "TS_3_FINGER_DRAG_RELEASING";
        break;
    }
    return "undefined";
}
time_t usec_diff(struct timeval *end, struct timeval *start) {
    return (end->tv_sec - start->tv_sec) * 1000000 + ((int) end->tv_usec - (int) start->tv_usec);
}
void calculate_elapsed_useconds_and_active_slots(struct State *state, struct timeval *time) {
    int i;
    state->active_slots = 0;
    for (i = 0; i < MAX_SLOTS; ++i) {
        if (state->slots[i].active) {
            state->active_slots++;
            state->slots[i].elapsed_useconds = usec_diff(time, &state->slots[i].start_time);
        } else {
            state->slots[i].elapsed_useconds = 0;
        }
    }
}
void clear_state(struct State *state) {
    int i;
    state->current_slot_id = 0;
    state->active_slots = 0;
    state->prev_active_slots = 0;
    state->touchpad_state = TS_DEFAULT;
    state->touchpad_state_updated_at.tv_sec = 0;
    state->touchpad_state_updated_at.tv_usec = 0;
    state->timer = NULL;
    clear_slot(&state->momentum_slot1);
    clear_slot(&state->momentum_slot2);
    for (i = 0; i < MAX_SLOTS; ++i) {
        clear_slot(&state->slots[i]);
        clear_slot(&state->prev_slots[i]);
    }
}
void clear_slot(struct Slot *slot) {
    slot->active = 0;
    slot->x = MAXINT;
    slot->y = MAXINT;
    slot->pressure = 0;
    slot->touch_major = 0;
    slot->touch_minor = 0;
    slot->width_major = 0;
    slot->width_minor = 0;
    slot->orientation = 0;

    slot->start_time.tv_sec = 0;
    slot->start_time.tv_usec = 0;
    slot->elapsed_useconds = 0;

    slot->startx = MAXINT;
    slot->starty = MAXINT;

    slot->ddx = 0.0;
    slot->ddy = 0.0;
    slot->delta_ddx = 0.0;
    slot->delta_ddy = 0.0;
    slot->dx = 0;
    slot->dy = 0;
    slot->total_dx = 0;
    slot->total_dy = 0;
}
void activate_current_slot(struct State *state, struct timeval *time) {
    state->slots[state->current_slot_id].active = 1;
    if (state->slots[state->current_slot_id].start_time.tv_sec == 0) {
        state->slots[state->current_slot_id].start_time = *time;
    }
}
int get_active_slot_id(struct Slot slots[]) {
    int i;
    for (i = 0; i < MAX_SLOTS; ++i) {
        if (slots[i].active) {
            return i;
        }
    }
    PRINT_WARN("No active slot!\n");
    return -1;
}
void get_2_active_slots(struct Slot slots[], struct Slot **slot1, struct Slot **slot2) {
    int i;
    *slot1 = NULL;
    *slot2 = NULL;
    for (i = 0; i < MAX_SLOTS; ++i) {
        if (slots[i].active) {
            if (*slot1 == NULL) {
                *slot1 = &slots[i];
            } else {
                *slot2 = &slots[i];
                break;
            }
        }
    }
}
void get_3_active_slots(struct Slot slots[], struct Slot **slot1, struct Slot **slot2, struct Slot **slot3) {
    int i;
    *slot1 = NULL;
    *slot2 = NULL;
    *slot3 = NULL;
    for (i = 0; i < MAX_SLOTS; ++i) {
        if (slots[i].active) {
            if (*slot1 == NULL) {
                *slot1 = &slots[i];
            } else if (*slot2 == NULL) {
                *slot2 = &slots[i];
            } else {
                *slot3 = &slots[i];
                break;
            }
        }
    }
}
int is_tap_click(struct Slot *slot) {
    if (!slot->active) {
        PRINT_DEBUG("is_tap_click: slot is not active\n");
        return 0;
    }
    if (slot->elapsed_useconds > 150000) {
        PRINT_DEBUG("is_tap_click: elapsed_useconds is too much: %i\n", slot->elapsed_useconds);
        return 0;
    }
    if (slot->total_dx > 2 || slot->total_dy > 2) {
        PRINT_DEBUG("is_tap_click: movement is too much, dx: %i, dy: %i\n", slot->total_dx, slot->total_dy);
        return 0;
    }
    return 1;
}
void set_start_fields_if_not_set(struct Slot *slot, struct timeval *time) {
    if (!slot->active) {
        return;
    }
    if (slot->startx == MAXINT) {
        slot->startx = slot->x;
    }
    if (slot->starty == MAXINT) {
        slot->starty = slot->y;
    }
}
void calculate_dx_dy(struct Slot *slot, struct Slot *prev_slot, struct timeval *time) {
    double speed;
    int delta;

    if (!slot->active) {
        return;
    }
    delta = abs(slot->x - prev_slot->x) + abs(slot->y - prev_slot->y);
    if (delta == 0) {
        speed = 25.0;
    } else {
        speed = pow((slot->elapsed_useconds - prev_slot->elapsed_useconds) / (double) delta, 0.7) * 0.5;
        if (speed > 25.0) {
            speed = 25.0;
        } else if (speed < 5.0) {
            speed = 5.0;
        }
    }
    if (slot->x != MAXINT && prev_slot->x != MAXINT) {
        slot->ddx += (slot->x - prev_slot->x) / speed;
        if (abs(slot->pressure - prev_slot->pressure) > 20 || slot->elapsed_useconds < 15000) { // sudden change in pressure, the user releasing the touchpad
            slot->ddx = 0.0;
        }
        slot->dx = (int) slot->ddx;
        if (slot->dx != 0) {
            slot->total_dx += abs(slot->dx);
            slot->ddx -= slot->dx;
        }
    }
    if (slot->y != MAXINT && prev_slot->y != MAXINT) {
        slot->ddy += (slot->y - prev_slot->y) / speed;
        if (abs(slot->pressure - prev_slot->pressure) > 20 || slot->elapsed_useconds < 15000) { // sudden change in pressure, the user releasing the touchpad
            slot->ddy = 0.0;
        }
        slot->dy = (int) slot->ddy;
        if (slot->dy != 0) {
            slot->total_dy += abs(slot->dy);
            slot->ddy -= slot->dy;
        }
    }
}
void update_touchpad_state(struct State *state, enum TouchpadStates new_state, struct timeval *time) {
    update_touchpad_state_msg(state, new_state, time, NULL);
}
void update_touchpad_state_msg(struct State *state, enum TouchpadStates new_state, struct timeval *time, const char *msg) {
    if (state->touchpad_state != new_state) {
        PRINT_INFO("update_touchpad_state %s => %s %s\n", touchpad_state_name(state->touchpad_state), touchpad_state_name(new_state), msg ? msg : "");
        state->touchpad_state = new_state;
        state->touchpad_state_updated_at = *time;
    }
}
void debug_slots(struct State *state) {
    PRINT_DEBUG("active: %i (%i), state: %s, slots: (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i)\n",
        state->active_slots, state->prev_active_slots, touchpad_state_name(state->touchpad_state),
        state->slots[0].active ? "*" : "-", state->slots[0].active ? state->slots[0].x : 0, state->slots[0].active ? state->slots[0].y : 0, state->slots[0].elapsed_useconds / 1000, state->slots[0].pressure,
        state->slots[1].active ? "*" : "-", state->slots[1].active ? state->slots[1].x : 0, state->slots[1].active ? state->slots[1].y : 0, state->slots[1].elapsed_useconds / 1000, state->slots[1].pressure,
        state->slots[2].active ? "*" : "-", state->slots[2].active ? state->slots[2].x : 0, state->slots[2].active ? state->slots[2].y : 0, state->slots[2].elapsed_useconds / 1000, state->slots[2].pressure,
        state->slots[3].active ? "*" : "-", state->slots[3].active ? state->slots[3].x : 0, state->slots[3].active ? state->slots[3].y : 0, state->slots[3].elapsed_useconds / 1000, state->slots[3].pressure,
        state->slots[4].active ? "*" : "-", state->slots[4].active ? state->slots[4].x : 0, state->slots[4].active ? state->slots[4].y : 0, state->slots[4].elapsed_useconds / 1000, state->slots[4].pressure
        );

    // touch_mul      = state->slots[0].touch_major * state->slots[0].touch_minor;
    // width_mul      = state->slots[0].width_major * state->slots[0].width_minor;
    // prev_touch_mul = state->prev_slots[0].touch_major * state->prev_slots[0].touch_minor;
    // prev_width_mul = state->prev_slots[0].width_major * state->prev_slots[0].width_minor;
    // if (touch_mul == 0) {
    //     touch_mul = 1;
    // }
    // if (width_mul == 0) {
    //     width_mul = 1;
    // }
    // if (prev_touch_mul == 0) {
    //     prev_touch_mul = touch_mul;
    // }
    // if (prev_width_mul == 0) {
    //     prev_width_mul = width_mul;
    // }
    // PRINT_INFO("active: %i, slot 0: (%s %i:%i (%3i:%3i) %umsec, pressure: %i, touch: %i/%i (%i d: %.2f%%), width: %i/%i (%i d: %.2f%%), o: %i)\n",
    //     state->active_slots,
    //     state->slots[0].active ? "*" : "-",
    //     state->slots[0].x, state->slots[0].y,
    //     state->slots[0].dx, state->slots[0].dy,
     //     state->slots[0].elapsed_useconds / 1000,
    //     state->slots[0].pressure,
    //     state->slots[0].touch_major, state->slots[0].touch_minor, state->slots[0].touch_major * state->slots[0].touch_minor, 100.0 * ((state->slots[0].touch_major * state->slots[0].touch_minor) / prev_touch_mul),
    //     state->slots[0].width_major, state->slots[0].width_minor, state->slots[0].width_major * state->slots[0].width_minor, 100.0 * ((state->slots[0].width_major * state->slots[0].width_minor) / prev_width_mul),
    //     state->slots[0].orientation);
}
void do_scrolling(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct timeval *time, int is_momentum) {
    int dx, dy, button, i;

    slot1->ddx += slot1->delta_ddx;
    slot1->ddy += slot1->delta_ddy;
    slot2->ddx = slot1->ddx;
    slot2->ddy = slot1->ddy;
    dx = (int) slot1->ddx;
    dy = (int) slot1->ddy;
    if (abs(dx) > 10) {
        dx = 0;
    }
    if (abs(dy) > 10) {
        dy = 0;
    }
    if (dx != 0) {
        PRINT_INFO("Horizontal scroll %i, delta_ddx: %f\n", dx, slot1->delta_ddx);
        button = (dx > 0) ? MOUSE_HORIZONTAL_WHEEL_1_BUTTON : MOUSE_HORIZONTAL_WHEEL_2_BUTTON;
        for (i = 0; i < abs(dx); ++i) {
            xf86PostButtonEvent(pInfo->dev, FALSE, button, TRUE, 0, 0);
            xf86PostButtonEvent(pInfo->dev, FALSE, button, FALSE, 0, 0);
        }
        slot1->ddx -= dx;
        slot2->ddx = slot1->ddx;
        if (!is_momentum) {
            update_touchpad_state(state, TS_2_FINGER_SCROLL, time);
        }
    }
    if (dy != 0) {
        PRINT_INFO("Vertical scroll %i, delta_ddy: %f\n", dy, slot1->delta_ddy);
        button = (dy > 0) ? MOUSE_VERTICAL_WHEEL_1_BUTTON : MOUSE_VERTICAL_WHEEL_2_BUTTON;
        for (i = 0; i < abs(dy); ++i) {
            xf86PostButtonEvent(pInfo->dev, FALSE, button, TRUE, 0, 0);
            xf86PostButtonEvent(pInfo->dev, FALSE, button, FALSE, 0, 0);
        }
        slot1->ddy -= dy;
        slot2->ddy = slot1->ddy;
        if (!is_momentum) {
            update_touchpad_state(state, TS_2_FINGER_SCROLL, time);
        }
    }
}
void handle_2_finger_scroll(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *prev_slot1, struct Slot *prev_slot2, struct timeval *time) {
    int x, y, prevx, prevy;
    set_start_fields_if_not_set(slot1, time);
    set_start_fields_if_not_set(slot2, time);
    x = (slot1->x + slot2->x) / 2;
    y = (slot1->y + slot2->y) / 2;
    prevx = (prev_slot1->x + prev_slot2->x) / 2;
    prevy = (prev_slot1->y + prev_slot2->y) / 2;
    slot1->delta_ddx = (x - prevx) / 200.0;
    slot1->delta_ddy = (y - prevy) / 200.0;
    if (fabs(slot1->delta_ddx) > 4.0 * fabs(slot1->delta_ddy)) {
        slot1->delta_ddy = 0;
    }
    if (fabs(slot1->delta_ddy) > 4.0 * fabs(slot1->delta_ddx)) {
        slot1->delta_ddx = 0;
    }
    PRINT_DEBUG("handle_2_finger_scroll delta_ddx: %f, delta_ddy: %f\n", slot1->delta_ddx, slot1->delta_ddy);
    do_scrolling(pInfo, state, slot1, slot2, time, FALSE);
}
void handle_3_finger_drag(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *slot3, struct Slot *prev_slot1, struct Slot *prev_slot2, struct Slot *prev_slot3, struct timeval *time) {
    int dx, dy;
    set_start_fields_if_not_set(slot1, time);
    set_start_fields_if_not_set(slot2, time);
    set_start_fields_if_not_set(slot3, time);
    calculate_dx_dy(slot1, prev_slot1, time);
    calculate_dx_dy(slot2, prev_slot2, time);
    calculate_dx_dy(slot3, prev_slot3, time);
    dx = round((slot1->dx + slot2->dx + slot3->dx) / 3.0);
    dy = round((slot1->dy + slot2->dy + slot3->dy) / 3.0);
    if (dx != 0 || dy != 0) {
        if (state->touchpad_state != TS_3_FINGER_DRAG) {
            update_touchpad_state(state, TS_3_FINGER_DRAG, time);
            xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, TRUE, 0, 0);
        }
        xf86PostMotionEvent(pInfo->dev, 0, 0, 2, dx, dy);
    }
}
static CARD32 timerFunc(OsTimerPtr timer, CARD32 now, pointer arg) {
    InputInfoPtr pInfo;
    RandomDevicePtr pRandom;
    struct State *state;
    struct timeval time;

    pInfo = arg;
    pRandom = pInfo->private;
    state = &pRandom->state;
    if (state->touchpad_state == TS_3_FINGER_DRAG_RELEASING) {
        time.tv_sec = 0;
        time.tv_usec = 0;
        update_touchpad_state_msg(state, TS_DEFAULT, &time, "Cancel 3 finger drag");
        xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, FALSE, 0, 0);
    }
    return 0;
}
static CARD32 timerfunc_scroll_momentum(OsTimerPtr timer, CARD32 now, pointer arg) {
    InputInfoPtr pInfo;
    RandomDevicePtr pRandom;
    struct State *state;
    struct timeval time;

    pInfo = arg;
    pRandom = pInfo->private;
    state = &pRandom->state;
    if (state->touchpad_state == TS_2_FINGER_SCROLL_MOMENTUM) {
        time.tv_sec = 0;
        time.tv_usec = 0;
        do_scrolling(pInfo, state, &state->momentum_slot1, &state->momentum_slot2, &time, TRUE);
        if ((fabs(state->momentum_slot1.delta_ddx) >= MOMENTUM_DELTA_LIMIT) || (fabs(state->momentum_slot1.delta_ddy) >= MOMENTUM_DELTA_LIMIT)) {
            state->momentum_slot1.delta_ddx *= 0.97;
            state->momentum_slot1.delta_ddy *= 0.97;
            state->timer = TimerSet(state->timer, 0, 10, timerfunc_scroll_momentum, pInfo);
        } else {
            PRINT_DEBUG("Not enough momentum! delta_ddx: %f, delta_ddy: %f\n", state->momentum_slot1.delta_ddx, state->momentum_slot1.delta_ddy);
            update_touchpad_state_msg(state, TS_DEFAULT, &time, "Scroll momentum is not enough");
        }
    }
    return 0;
}
void process_EV_SYN(InputInfoPtr pInfo, struct State *state, struct timeval *time) {
    int i;
    struct Slot *slot, *slot1, *slot2, *slot3;
    struct Slot *prev_slot, *prev_slot1, *prev_slot2, *prev_slot3;
    if (state->touchpad_state == TS_3_FINGER_DRAG && state->active_slots != 3) {
        update_touchpad_state(state, TS_3_FINGER_DRAG_RELEASING, time);
        state->timer = TimerSet(state->timer, 0, 500, timerFunc, pInfo);
    } else if (state->touchpad_state == TS_3_FINGER_DRAG_RELEASING) {
        if (state->active_slots == 3) {
            update_touchpad_state(state, TS_3_FINGER_DRAG, time);
            TimerCancel(state->timer);
        } else {
            // keep waiting until timer kills the current state or the user reconnects the 3 fingers
        }
    } else if (state->touchpad_state == TS_2_FINGER_SCROLL && state->active_slots < 2) {
        update_touchpad_state(state, TS_2_FINGER_SCROLL_RELEASING, time);
        if (state->prev_active_slots == 2) {
            get_2_active_slots(state->prev_slots, &prev_slot1, &prev_slot2);
            if (fabs(prev_slot1->delta_ddx) >= MOMENTUM_DELTA_LIMIT_2X || fabs(prev_slot1->delta_ddy) >= MOMENTUM_DELTA_LIMIT_2X) {
                state->momentum_slot1 = *prev_slot1;
                state->momentum_slot2 = *prev_slot2;
                if (fabs(state->momentum_slot1.delta_ddx) < MOMENTUM_DELTA_LIMIT_2X) {
                    state->momentum_slot1.delta_ddx = 0;
                    state->momentum_slot2.delta_ddx = 0;
                }
                if (fabs(state->momentum_slot1.delta_ddy) < MOMENTUM_DELTA_LIMIT_2X) {
                    state->momentum_slot1.delta_ddy = 0;
                    state->momentum_slot2.delta_ddy = 0;
                }
                update_touchpad_state(state, TS_2_FINGER_SCROLL_MOMENTUM, time);
                PRINT_INFO("start scroll momentum delta_ddx: %f, delta_ddy: %f\n", state->momentum_slot1.delta_ddx, state->momentum_slot1.delta_ddy);
                timerfunc_scroll_momentum(state->timer, 0, pInfo);
            }
        }
    } else if (state->touchpad_state == TS_2_FINGER_SCROLL && state->active_slots > 2) {
        update_touchpad_state_msg(state, TS_DEFAULT, time, "Cancel 2 finger scroll because more than 2 fingers touched");
    } else if (state->touchpad_state == TS_2_FINGER_SCROLL_MOMENTUM) {
        // momentum is emulated by timer, but user can stop it with 2 fingers
        if (state->active_slots >= 2) {
            update_touchpad_state_msg(state, TS_DEFAULT, time, "Cancel 2 finger scroll momentum with 2 or more fingers");
            TimerCancel(state->timer);
        } else if (state->active_slots == 1) {
            i = get_active_slot_id(state->slots);
            slot = &state->slots[i];
            if (slot->elapsed_useconds >= 50000 && usec_diff(time, &state->touchpad_state_updated_at) > 50000) {
                PRINT_DEBUG("Scroll momentum cancelled by holding 1 finger for %d msec\n", slot->elapsed_useconds / 1000);
                update_touchpad_state_msg(state, TS_DEFAULT, time, "Cancel scroll momentum with 1 finger");
                TimerCancel(state->timer);
            }
        }
    } else if (state->active_slots == 1) {
        i = get_active_slot_id(state->slots);
        slot = &state->slots[i];
        prev_slot = &state->prev_slots[i];

        if (state->touchpad_state == TS_2_FINGER_SCROLL) {
            // this should never happen, as this state is handled above
        } else if (state->touchpad_state == TS_2_FINGER_SCROLL_RELEASING) {
            if (usec_diff(time, &state->touchpad_state_updated_at) < 100000) {
                // keep waiting
            } else {
                update_touchpad_state_msg(state, TS_DEFAULT, time, "Switching to normal mouse moving");
                // overwrite the start position
                slot->startx = slot->x;
                slot->starty = slot->y;
            }
        } else {
            set_start_fields_if_not_set(slot, time);
            calculate_dx_dy(slot, prev_slot, time);
            if (slot->dx != 0 || slot->dy != 0) {
                xf86PostMotionEvent(pInfo->dev, 0, 0, 2, slot->dx, slot->dy);
            }
        }
    } else if (state->active_slots == 0 && state->prev_active_slots == 1) {
        i = get_active_slot_id(state->prev_slots);
        if (state->touchpad_state == TS_2_FINGER_SCROLL_RELEASING) {
            update_touchpad_state_msg(state, TS_DEFAULT, time, "No more fingers touching");
        }
        if (i >= 0) {
            prev_slot = &state->prev_slots[i];
            if (is_tap_click(prev_slot)) {
                PRINT_INFO("Tap to click\n");
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, TRUE, 0, 0);
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, FALSE, 0, 0);
            }
        } else {
            PRINT_WARN("No active prev_slot! slots: (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec)\n",
                state->prev_slots[0].active ? "*" : "-", state->prev_slots[0].x, state->prev_slots[0].y, state->prev_slots[0].elapsed_useconds/1000,
                state->prev_slots[1].active ? "*" : "-", state->prev_slots[1].x, state->prev_slots[1].y, state->prev_slots[1].elapsed_useconds/1000,
                state->prev_slots[2].active ? "*" : "-", state->prev_slots[2].x, state->prev_slots[2].y, state->prev_slots[2].elapsed_useconds/1000,
                state->prev_slots[3].active ? "*" : "-", state->prev_slots[3].x, state->prev_slots[3].y, state->prev_slots[3].elapsed_useconds/1000,
                state->prev_slots[4].active ? "*" : "-", state->prev_slots[4].x, state->prev_slots[4].y, state->prev_slots[4].elapsed_useconds/1000
                );
        }
    } else if (state->active_slots == 2 && state->prev_active_slots == 2) {
        get_2_active_slots(state->slots, &slot1, &slot2);
        if (slot1 != NULL && slot2 != NULL) {
            get_2_active_slots(state->prev_slots, &prev_slot1, &prev_slot2);
            if (prev_slot1 != NULL && prev_slot2 != NULL) {
                handle_2_finger_scroll(pInfo, state, slot1, slot2, prev_slot1, prev_slot2, time);
            }
        }
    } else if (state->active_slots == 3 && state->prev_active_slots == 3) {
        get_3_active_slots(state->slots, &slot1, &slot2, &slot3);
        if (slot1 != NULL && slot2 != NULL && slot3 != NULL) {
            get_3_active_slots(state->prev_slots, &prev_slot1, &prev_slot2, &prev_slot3);
            if (prev_slot1 != NULL && prev_slot2 != NULL && prev_slot3 != NULL) {
                handle_3_finger_drag(pInfo, state, slot1, slot2, slot3, prev_slot1, prev_slot2, prev_slot3, time);
            }
        }
    } else {
        PRINT_INFO("Unhandled case in process_EV_SYN! touchpad_state: %i, active_slots: %i, prev_active_slots: %i\n",
            state->touchpad_state, state->active_slots, state->prev_active_slots);
        if (state->touchpad_state != TS_DEFAULT && usec_diff(time, &state->touchpad_state_updated_at) > 3000000) {
            PRINT_INFO("State was stuck to %i, resetting it to TS_DEFAULT.\n", state->touchpad_state);
            update_touchpad_state_msg(state, TS_DEFAULT, time, "Unlock stucked state");
        }
    }
}
void save_current_values_to_prev(struct State *state) {
    int i;
    state->prev_active_slots = state->active_slots;
    for (i = 0; i < MAX_SLOTS; ++i) {
        state->prev_slots[i] = state->slots[i];
    }
}
void process_event(InputInfoPtr pInfo, struct State *state, struct timeval *time, int type, int code, int value) {
    switch (type) {
        case EV_SYN:
        calculate_elapsed_useconds_and_active_slots(state, time);
        debug_slots(state);
        process_EV_SYN(pInfo, state, time);
        save_current_values_to_prev(state);
        break;
        case EV_KEY:
        switch (code) {
            case BTN_LEFT:
            if (state->active_slots == 1) {
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, value, 0, 0);
            } else if (state->active_slots == 2) {
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_RIGHT_BUTTON, value, 0, 0);
            }
            break;
            case BTN_TOOL_FINGER:
            break;
            case BTN_TOOL_QUINTTAP:
            break;
            case BTN_TOUCH:
            break;
            case BTN_TOOL_DOUBLETAP:
            break;
            case BTN_TOOL_TRIPLETAP:
            break;
            case BTN_TOOL_QUADTAP:
            break;
        }
        break;
        case EV_ABS:
        switch (code) {
            case ABS_X:
            break;
            case ABS_Y:
            break;
            case ABS_PRESSURE:
            state->slots[state->current_slot_id].pressure = value;
            break;
            case ABS_TOOL_WIDTH:
            break;
            case ABS_MT_SLOT:
            set_start_fields_if_not_set(&state->slots[state->current_slot_id], time);
            state->current_slot_id = value;
            activate_current_slot(state, time);
            break;
            case ABS_MT_TOUCH_MAJOR:
            state->slots[state->current_slot_id].touch_major = value;
            break;
            case ABS_MT_TOUCH_MINOR:
            state->slots[state->current_slot_id].touch_minor = value;
            break;
            case ABS_MT_WIDTH_MAJOR:
            state->slots[state->current_slot_id].width_major = value;
            break;
            case ABS_MT_WIDTH_MINOR:
            state->slots[state->current_slot_id].width_minor = value;
            break;
            case ABS_MT_ORIENTATION:
            state->slots[state->current_slot_id].orientation = value;
            break;
            case ABS_MT_POSITION_X:
            state->slots[state->current_slot_id].x = value;
            break;
            case ABS_MT_POSITION_Y:
            state->slots[state->current_slot_id].y = value;
            break;
            case ABS_MT_TRACKING_ID:
            if (value < 0) {
                clear_slot(&state->slots[state->current_slot_id]);
            } else {
                activate_current_slot(state, time);
            }
            break;
        }
        break;
    }
    if (type != EV_SYN) {
        PRINT_DEBUG("data: %zu %8zu %6i %s\n", time->tv_sec, time->tv_usec, value, type_and_code_name(type, code));
    }
}

static void RandomReadInput(InputInfoPtr pInfo)
{
    RandomDevicePtr pRandom = pInfo->private;
    struct input_event ev;
    int res;

    while (1) {
        res = libevdev_next_event(pRandom->evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (res < 0) {
            if (res == -EAGAIN) {
                break;
            } else {
                xf86Msg(X_ERROR, "Cannot read next event: %i\n", res);
                break;
            }
        } else {
            process_event(pInfo, &pRandom->state, &ev.time, ev.type, ev.code, ev.value);
        }
    }
}

