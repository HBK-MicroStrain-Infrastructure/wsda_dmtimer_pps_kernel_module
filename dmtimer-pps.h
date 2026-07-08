// SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note
// Copyright (C) 2026 MicroStrain by HBK <ext_jonathan.herbst@hbkworld.com>

#ifndef _DMTIMER_PPS_H_
#define _DMTIMER_PPS_H_

#include <linux/types.h>

// Capture a pps on the rising edge
#define DMTIMER_PPS_MODE_INPUT_CAPTURE_RISING 0x00
// Capture a pps on the falling edge
#define DMTIMER_PPS_MODE_INPUT_CAPTURE_FALLING 0x01
// Output a pps
#define DMTIMER_PPS_MODE_OUTPUT 0x02


struct dmtimer_pps_params {
    // Mode of the dmtimer
    __u32 mode;
    // Period of the timer in counts
    __u32 period;
    // A value to adjust the period by for one cycle
    __s32 adjust;
    // Approximate frequency of the timer counter in Hz (ignored when writing)
    __u32 frequency;
};

// the timer event is not valid
#define DMTIMER_PPS_TYPE_INVALID 0
// The timer overflowed
#define DMTIMER_PPS_TYPE_OVERFLOW 1
// The timer hit its match value
#define DMTIMER_PPS_TYPE_MATCH 2
// The timer captured an external edge
#define DMTIMER_PPS_TYPE_CAPTURE 3

struct dmtimer_pps_event {
    // Type of event defined by DMTIMER_PPS_TYPE_*
    __u32 type;
    // Absolute timer counter when the event happened
    __u64 event_seq;
    // Absolute timer counter when the interrupt happened
    __u64 irq_seq;
    // CLOCK_REALTIME capture when the interrupt happened
    __u64 irq_realtime;
};

#include <linux/ioctl.h>

// we use the pps.h magic, 'p', but sequence numbers outside the pps.h block, A8-AF.
#define DMTIMER_PPS_GETPARAMS		_IOR('p', 0xA8, struct dmtimer_pps_params *)
#define DMTIMER_PPS_SETPARAMS		_IOW('p', 0xA9, struct dmtimer_pps_params *)

#endif // _DMTIMER_PPS_H_