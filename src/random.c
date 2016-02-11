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

static int RandomPreInit(InputDriverPtr  drv, InputInfoPtr pInfo, int flags);
static void RandomUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static pointer RandomPlug(pointer module, pointer options, int *errmaj, int  *errmin);
static void RandomUnplug(pointer p);
static void RandomReadInput(InputInfoPtr pInfo);
static int RandomControl(DeviceIntPtr    device,int what);
static int _random_init_buttons(DeviceIntPtr device);
static int _random_init_axes(DeviceIntPtr device);

const char *type_and_code_name(int type, int code);
time_t usec_diff(struct timeval *end, struct timeval *start);
void calculate_elapsed_useconds_and_active_slots(struct State *state, struct timeval *time);
void clear_state(struct State *state);
void clear_slot(struct Slot *slot);
void activate_current_slot(struct State *state, struct timeval *time);
int get_active_slot_id(struct Slot slots[]);
void get_2_active_slots(struct Slot slots[], struct Slot **slot1, struct Slot **slot2);
int is_tap_click(struct Slot *slot);
void set_start_fields_if_not_set(struct Slot *slot, struct timeval *time);
void calculate_dx_dy(struct Slot *slot, struct Slot *prev_slot, struct timeval *time);
void update_touchpad_state(struct State *state, enum TouchpadStates new_state, struct timeval *time);
void debug_slots(struct State *state);
void handle_2_finger_scroll(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *prev_slot1, struct Slot *prev_slot2, struct timeval *time);
void process_EV_SYN(InputInfoPtr pInfo, struct State *state, struct timeval *time);
void save_current_values_to_prev(struct State *state);
void process_event(InputInfoPtr pInfo, struct State *state, struct timeval *time, int type, int code, int value);

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

    xf86Msg(X_INFO, "%s: Using device %s.\n", pInfo->name, pRandom->device);

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
            xf86Msg(X_INFO, "%s: On.\n", pInfo->name);
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
            xf86Msg(X_INFO, "%s: Off.\n", pInfo->name);
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
    xf86Msg(X_ERROR, "No active slot!\n");
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
int is_tap_click(struct Slot *slot) {
    if (!slot->active) {
        // xf86Msg(X_INFO, "is_tap_click: slot is not active\n");
        return 0;
    }
    if (slot->elapsed_useconds > 100000) {
        // xf86Msg(X_INFO, "is_tap_click: elapsed_useconds is too much: %i\n", slot->elapsed_useconds);
        return 0;
    }
    if (slot->total_dx > 0 || slot->total_dy > 0) {
        // xf86Msg(X_INFO, "is_tap_click: movement is too much, dx: %i, dy: %i\n", slot->total_dx, slot->total_dy);
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
    double speed, delta;

    if (!slot->active) {
        return;
    }
    if (slot->pressure == 0) {
        return;
    }
    delta = abs(slot->x - prev_slot->x) + abs(slot->y - prev_slot->y);
    if (delta == 0) {
        speed = 25.0;
    } else {
        speed = pow((slot->elapsed_useconds - prev_slot->elapsed_useconds) / delta, 0.7) * 0.5;
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
    if (state->touchpad_state != new_state) {
        state->touchpad_state = new_state;
        state->touchpad_state_updated_at = *time;
    }
}
void debug_slots(struct State *state) {
    xf86Msg(X_INFO, "active: %i, slots: (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i) (%s %i:%i %ums %i)\n",
        state->active_slots,
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
    // xf86Msg(X_INFO, "active: %i, slot 0: (%s %i:%i (%3i:%3i) %umsec, pressure: %i, touch: %i/%i (%i d: %.2f%%), width: %i/%i (%i d: %.2f%%), o: %i)\n",
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
void handle_2_finger_scroll(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *prev_slot1, struct Slot *prev_slot2, struct timeval *time) {
    int x, y, prevx, prevy, dx, dy, button, i;
    double ddx, ddy;
    set_start_fields_if_not_set(slot1, time);
    set_start_fields_if_not_set(slot2, time);
    x = (slot1->x + slot2->x) / 2;
    y = (slot1->y + slot2->y) / 2;
    prevx = (prev_slot1->x + prev_slot2->x) / 2;
    prevy = (prev_slot1->y + prev_slot2->y) / 2;
    ddx = (x - prevx) / 200.0;
    ddy = (y - prevy) / 200.0;
    slot1->ddx += ddx;
    slot1->ddy += ddy;
    slot2->ddx = slot1->ddx;
    slot2->ddy = slot1->ddy;
    dx = (int) slot1->ddx;
    dy = (int) slot1->ddy;
    if (abs(dx) > 5) {
        dx = 0;
    }
    if (abs(dy) > 5) {
        dy = 0;
    }
    if (dx != 0) {
        xf86Msg(X_INFO, "Horizontal scroll %i\n", dx);
        button = (dx > 0) ? MOUSE_HORIZONTAL_WHEEL_1_BUTTON : MOUSE_HORIZONTAL_WHEEL_2_BUTTON;
        for (i = 0; i < abs(dx); ++i) {
            xf86PostButtonEvent(pInfo->dev, FALSE, button, TRUE, 0, 0);
            xf86PostButtonEvent(pInfo->dev, FALSE, button, FALSE, 0, 0);
        }
        slot1->ddx -= dx;
        slot2->ddx = slot1->ddx;
        update_touchpad_state(state, TS_2_FINGER_SCROLL, time);
    }
    if (dy != 0) {
        xf86Msg(X_INFO, "Vertical scroll %i\n", dy);
        button = (dy > 0) ? MOUSE_VERTICAL_WHEEL_1_BUTTON : MOUSE_VERTICAL_WHEEL_2_BUTTON;
        for (i = 0; i < abs(dy); ++i) {
            xf86PostButtonEvent(pInfo->dev, FALSE, button, TRUE, 0, 0);
            xf86PostButtonEvent(pInfo->dev, FALSE, button, FALSE, 0, 0);
        }
        slot1->ddy -= dy;
        slot2->ddy = slot1->ddy;
        update_touchpad_state(state, TS_2_FINGER_SCROLL, time);
    }
}
void process_EV_SYN(InputInfoPtr pInfo, struct State *state, struct timeval *time) {
    int i;
    struct Slot *slot, *slot1, *slot2;
    struct Slot *prev_slot, *prev_slot1, *prev_slot2;
    if (state->active_slots == 1) {
        i = get_active_slot_id(state->slots);
        slot = &state->slots[i];
        prev_slot = &state->prev_slots[i];

        if (state->touchpad_state == TS_2_FINGER_SCROLL) {
            update_touchpad_state(state, TS_2_FINGER_SCROLL_RELEASING, time);
        } else if (state->touchpad_state == TS_2_FINGER_SCROLL_RELEASING) {
            if (usec_diff(time, &state->touchpad_state_updated_at) < 100000) {
                // keep waiting
            } else {
                update_touchpad_state(state, TS_DEFAULT, time);
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
        if (i >= 0) {
            prev_slot = &state->prev_slots[i];
            if (is_tap_click(prev_slot)) {
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, TRUE, 0, 0);
                xf86PostButtonEvent(pInfo->dev, FALSE, MOUSE_LEFT_BUTTON, FALSE, 0, 0);
            }
        } else {
            xf86Msg(X_ERROR, "No active prev_slot! slots: (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec)\n",
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
        xf86Msg(X_INFO, "data: %zu %8zu %6i %s\n", time->tv_sec, time->tv_usec, value, type_and_code_name(type, code));
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

