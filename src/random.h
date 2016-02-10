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

#define MAX_SLOTS 5
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

    unsigned int start_seconds;
    unsigned int start_useconds;
    int elapsed_useconds;

    int startx;
    int starty;

    double ddx;
    double ddy;
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

