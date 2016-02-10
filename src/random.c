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
unsigned int elapsed_millis(struct Slot slot, unsigned int seconds, unsigned int useconds);
unsigned int elapsed_useconds(struct Slot *slot, unsigned int seconds, unsigned int useconds);
void clear_state(struct State *state);
void clear_slot(struct Slot *slot);
void activate_current_slot(struct State *state, unsigned int seconds, unsigned int useconds);
void set_start_fields_if_not_set(struct Slot *slot, unsigned int seconds, unsigned int useconds);
void calculate_dx_dy(struct Slot *slot, struct Slot *prev_slot, unsigned int seconds, unsigned int useconds);
void process_event(InputInfoPtr pInfo, struct State *state, unsigned int seconds, unsigned int useconds, int type, int code, int value);

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
        /* Common error - pInfo->private must be NULL or valid memoy before
         * passing into xf86DeleteInput */
        pInfo->private = NULL;
    }
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
unsigned int elapsed_millis(struct Slot slot, unsigned int seconds, unsigned int useconds) {
    if (!slot.active) {
        return 0;
    }
    return (seconds - slot.start_seconds) * 1000 + (((int) useconds) - ((int) slot.start_useconds)) / 1000;
}
unsigned int elapsed_useconds(struct Slot *slot, unsigned int seconds, unsigned int useconds) {
    if (!slot->active) {
        return 0;
    }
    return (seconds - slot->start_seconds) * 1000000 + (((int) useconds) - ((int) slot->start_useconds));
}
void clear_state(struct State *state) {
    int i;
    state->current_slot_id = 0;
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

    slot->start_seconds = 0;
    slot->start_useconds = 0;
    slot->elapsed_useconds = 0;

    slot->startx = MAXINT;
    slot->starty = MAXINT;

    slot->ddx = 0.0;
    slot->ddy = 0.0;
    slot->dx = 0;
    slot->dy = 0;
}
void activate_current_slot(struct State *state, unsigned int seconds, unsigned int useconds) {
    state->slots[state->current_slot_id].active = 1;
    if (state->slots[state->current_slot_id].start_seconds == 0) {
        state->slots[state->current_slot_id].start_seconds = seconds;
        state->slots[state->current_slot_id].start_useconds = useconds;
    }
}
void set_start_fields_if_not_set(struct Slot *slot, unsigned int seconds, unsigned int useconds) {
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
void calculate_dx_dy(struct Slot *slot, struct Slot *prev_slot, unsigned int seconds, unsigned int useconds) {
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
        if (abs(slot->pressure - prev_slot->pressure) > 20) { // sudden change in pressure, the user releasing the touchpad
            slot->ddx = 0.0;
        }
        slot->dx = (int) slot->ddx;
        if (slot->dx != 0) {
            slot->ddx -= slot->dx;
        }
    }
    if (slot->y != MAXINT && prev_slot->y != MAXINT) {
        slot->ddy += (slot->y - prev_slot->y) / speed;
        if (abs(slot->pressure - prev_slot->pressure) > 20) { // sudden change in pressure, the user releasing the touchpad
            slot->ddy = 0.0;
        }
        slot->dy = (int) slot->ddy;
        if (slot->dy != 0) {
            slot->ddy -= slot->dy;
        }
    }
}
void process_event(InputInfoPtr pInfo, struct State *state, unsigned int seconds, unsigned int useconds, int type, int code, int value) {
    double touch_mul, width_mul;
    double prev_touch_mul, prev_width_mul;
    int i;

    switch (type) {
        case EV_SYN:
        // xf86Msg(X_INFO, "slots: (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec) (%s %i:%i %umsec)\n",
        //     state->slots[0].active ? "*" : "-", state->slots[0].x, state->slots[0].y, elapsed_millis(state->slots[0], seconds, useconds),
        //     state->slots[1].active ? "*" : "-", state->slots[1].x, state->slots[1].y, elapsed_millis(state->slots[1], seconds, useconds),
        //     state->slots[2].active ? "*" : "-", state->slots[2].x, state->slots[2].y, elapsed_millis(state->slots[2], seconds, useconds),
        //     state->slots[3].active ? "*" : "-", state->slots[3].x, state->slots[3].y, elapsed_millis(state->slots[3], seconds, useconds),
        //     state->slots[4].active ? "*" : "-", state->slots[4].x, state->slots[4].y, elapsed_millis(state->slots[4], seconds, useconds)
        //     );
        for (i = 0; i < MAX_SLOTS; ++i) {
            state->slots[i].elapsed_useconds = elapsed_useconds(&state->slots[i], seconds, useconds);
        }

        touch_mul      = state->slots[0].touch_major * state->slots[0].touch_minor;
        width_mul      = state->slots[0].width_major * state->slots[0].width_minor;
        prev_touch_mul = state->prev_slots[0].touch_major * state->prev_slots[0].touch_minor;
        prev_width_mul = state->prev_slots[0].width_major * state->prev_slots[0].width_minor;
        if (touch_mul == 0) {
            touch_mul = 1;
        }
        if (width_mul == 0) {
            width_mul = 1;
        }
        if (prev_touch_mul == 0) {
            prev_touch_mul = touch_mul;
        }
        if (prev_width_mul == 0) {
            prev_width_mul = width_mul;
        }
        xf86Msg(X_INFO, "active: %i, slot 0: (%s %i:%i (%3i:%3i) %umsec, pressure: %i, touch: %i/%i (%i d: %.2f%%), width: %i/%i (%i d: %.2f%%), o: %i)\n",
            state->active_slots,
            state->slots[0].active ? "*" : "-",
            state->slots[0].x, state->slots[0].y,
            state->slots[0].dx, state->slots[0].dy,
            state->slots[0].elapsed_useconds / 1000,
            state->slots[0].pressure,
            state->slots[0].touch_major, state->slots[0].touch_minor, state->slots[0].touch_major * state->slots[0].touch_minor, 100.0 * ((state->slots[0].touch_major * state->slots[0].touch_minor) / prev_touch_mul),
            state->slots[0].width_major, state->slots[0].width_minor, state->slots[0].width_major * state->slots[0].width_minor, 100.0 * ((state->slots[0].width_major * state->slots[0].width_minor) / prev_width_mul),
            state->slots[0].orientation);
        if (state->active_slots == 1 && state->slots[0].active) {
            set_start_fields_if_not_set(&state->slots[state->current_slot_id], seconds, useconds);
            calculate_dx_dy(&state->slots[state->current_slot_id], &state->prev_slots[state->current_slot_id], seconds, useconds);
            if (state->slots[0].dx != 0 || state->slots[0].dy != 0) {
                xf86PostMotionEvent(pInfo->dev, 0, 0, 2, state->slots[0].dx, state->slots[0].dy);
            }
        }
        state->prev_slots[0] = state->slots[0];
        break;
        case EV_KEY:
        switch (code) {
            case BTN_LEFT:
            if (value == 1) {
                xf86PostButtonEvent(pInfo->dev, FALSE, 1, TRUE, 0, 0);
            } else {
                xf86PostButtonEvent(pInfo->dev, FALSE, 1, FALSE, 0, 0);
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
            set_start_fields_if_not_set(&state->slots[state->current_slot_id], seconds, useconds);
            state->current_slot_id = value;
            activate_current_slot(state, seconds, useconds);
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
                clear_slot(&state->prev_slots[state->current_slot_id]);
                state->active_slots--;
            } else {
                state->active_slots++;
                activate_current_slot(state, seconds, useconds);
            }
            break;
        }
        break;
    }
    if (type != EV_SYN) {
        xf86Msg(X_INFO, "data: %u %8u %6i %s\n", seconds, useconds, value, type_and_code_name(type, code));
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
            process_event(pInfo, &pRandom->state, ev.time.tv_sec, ev.time.tv_usec, ev.type, ev.code, ev.value);
        }
    }
}

