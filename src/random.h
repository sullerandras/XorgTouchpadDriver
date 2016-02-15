/*
 * Copyright 2007 Peter Hutterer
 * Copyright 2009 Przemys¿aw Firszt
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

#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

#ifdef DEBUG
#define PRINT_WARN(...)  xf86Msg(X_WARNING, __VA_ARGS__)
#define PRINT_INFO(...)  xf86Msg(X_INFO,    __VA_ARGS__)
#define PRINT_DEBUG(...) xf86Msg(X_DEBUG,   __VA_ARGS__)
#else
#define PRINT_WARN(...)
#define PRINT_INFO(...)
#define PRINT_DEBUG(...)
#endif

#define MOUSE_LEFT_BUTTON               1
#define MOUSE_RIGHT_BUTTON              3
#define MOUSE_MIDDLE_BUTTON             2
#define MOUSE_VERTICAL_WHEEL_1_BUTTON   4
#define MOUSE_VERTICAL_WHEEL_2_BUTTON   5
#define MOUSE_HORIZONTAL_WHEEL_1_BUTTON 6
#define MOUSE_HORIZONTAL_WHEEL_2_BUTTON 7

#define MOMENTUM_DELTA_LIMIT 0.2f
#define MOMENTUM_DELTA_LIMIT_2X (2 * MOMENTUM_DELTA_LIMIT)

#define MAX_SLOTS 5

enum TouchpadStates {
    TS_DEFAULT,
    TS_2_FINGER_SCROLL, // there are 2 active slots
    TS_2_FINGER_SCROLL_RELEASING, // previously there were 2 active slots but now only 1. Ignoring pointer movements for a short period to avoid accidental pointer movements after scrolling with 2 fingers
    TS_2_FINGER_SCROLL_MOMENTUM,
    TS_3_FINGER_DRAG,
    TS_3_FINGER_DRAG_RELEASING,
};

struct Slot {
    int slot_id;
    int active;
    int x;
    int y;
    int pressure;
    int touch_major;
    int touch_minor;
    int width_major;
    int width_minor;
    int orientation;

    struct timeval start_time;
    int elapsed_useconds;

    int startx;
    int starty;

    double ddx;
    double ddy;
    double delta_ddx;
    double delta_ddy;
    int dx;
    int dy;
    int total_dx; // total horizontal movement since the slot is active
    int total_dy; // total vertical movement since the slot is active
};
struct State {
    struct Slot slots[MAX_SLOTS];
    struct Slot prev_slots[MAX_SLOTS];
    int current_slot_id;
    int active_slots;
    int prev_active_slots;
    enum TouchpadStates touchpad_state;
    struct timeval touchpad_state_updated_at;
    OsTimerPtr timer;
    struct Slot momentum_slot1;
    struct Slot momentum_slot2;
};

typedef struct _RandomDeviceRec
{
    char *device;
    int version;        /* Driver version */
    Atom* labels;
    int num_vals;
    int axes;
    struct libevdev* evdev;
    struct State state;
} RandomDeviceRec, *RandomDevicePtr ;

static int RandomPreInit(InputDriverPtr  drv, InputInfoPtr pInfo, int flags);
static void RandomUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags);
static pointer RandomPlug(pointer module, pointer options, int *errmaj, int  *errmin);
static void RandomUnplug(pointer p);
static void RandomReadInput(InputInfoPtr pInfo);
static int RandomControl(DeviceIntPtr    device,int what);
static int _random_init_buttons(DeviceIntPtr device);
static int _random_init_axes(DeviceIntPtr device);

const char *type_and_code_name(int type, int code);
const char *touchpad_state_name(enum TouchpadStates state);
time_t usec_diff(struct timeval *end, struct timeval *start);
void calculate_elapsed_useconds_and_active_slots(struct State *state, struct timeval *time);
void clear_state(struct State *state);
void clear_slot(struct Slot *slot);
void activate_current_slot(struct State *state, struct timeval *time);
int get_active_slot_id(struct Slot slots[]);
void get_2_active_slots(struct Slot slots[], struct Slot **slot1, struct Slot **slot2);
void get_3_active_slots(struct Slot slots[], struct Slot **slot1, struct Slot **slot2, struct Slot **slot3);
int is_tap_click(struct Slot *slot);
void set_start_fields_if_not_set(struct Slot *slot, struct timeval *time);
void calculate_dx_dy(struct Slot *slot, struct Slot *prev_slot, struct timeval *time);
void update_touchpad_state(struct State *state, enum TouchpadStates new_state, struct timeval *time);
void update_touchpad_state_msg(struct State *state, enum TouchpadStates new_state, struct timeval *time, const char *msg);
void debug_slots(struct State *state);
void do_scrolling(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct timeval *time, int is_momentum);
void handle_2_finger_scroll(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *prev_slot1, struct Slot *prev_slot2, struct timeval *time);
void handle_3_finger_drag(InputInfoPtr pInfo, struct State *state, struct Slot *slot1, struct Slot *slot2, struct Slot *slot3, struct Slot *prev_slot1, struct Slot *prev_slot2, struct Slot *prev_slot3, struct timeval *time);
void process_EV_SYN(InputInfoPtr pInfo, struct State *state, struct timeval *time);
void save_current_values_to_prev(struct State *state);
void process_event(InputInfoPtr pInfo, struct State *state, struct timeval *time, int type, int code, int value);
