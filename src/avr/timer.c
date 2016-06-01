// AVR timer interrupt scheduling code.
//
// Copyright (C) 2016  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <avr/interrupt.h> // TCNT1
#include "command.h" // shutdown
#include "irq.h" // irq_save
#include "sched.h" // sched_timer_kick
#include "timer.h" // timer_from_us


/****************************************************************
 * Low level timer code
 ****************************************************************/

// Return the number of clock ticks for a given number of microseconds
uint32_t
timer_from_us(uint32_t us)
{
    return us * (F_CPU / 1000000);
}

static inline uint16_t
timer_get(void)
{
    return TCNT1;
}

static inline void
timer_set(uint16_t next)
{
    OCR1A = next;
}

static inline void
timer_set_clear(uint16_t next)
{
    OCR1A = next;
    TIFR1 = 1<<OCF1A;
}

ISR(TIMER1_COMPA_vect)
{
    sched_timer_kick();
}

static void
timer_init(void)
{
    // no outputs
    TCCR1A = 0;
    // Normal Mode
    TCCR1B = 1<<CS10;
    // enable interrupt
    TIMSK1 = 1<<OCIE1A;
}
DECL_INIT(timer_init);


/****************************************************************
 * 32bit timer wrappers
 ****************************************************************/

static uint32_t timer_last;

// Return the 32bit current time given the 16bit current time.
static __always_inline uint32_t
calc_time(uint32_t last, uint16_t cur)
{
    union u32_u16_u {
        struct { uint16_t lo, hi; };
        uint32_t val;
    } calc;
    calc.val = last;
    if (cur < calc.lo)
        calc.hi++;
    calc.lo = cur;
    return calc.val;
}

// Called by main code once every millisecond.  (IRQs disabled.)
void
timer_periodic(void)
{
    timer_last = calc_time(timer_last, timer_get());
}

// Return the current time (in absolute clock ticks).
uint32_t
timer_read_time(void)
{
    uint8_t flag = irq_save();
    uint16_t cur = timer_get();
    uint32_t last = timer_last;
    irq_restore(flag);
    return calc_time(last, cur);
}

#define TIMER_MIN_TICKS 100

// Set the next timer wake time (in absolute clock ticks).  Caller
// must disable irqs.  The caller should not schedule a time more than
// a few milliseconds in the future.
uint8_t
timer_set_next(uint32_t next)
{
    uint16_t cur = timer_get();
    if ((int16_t)(OCR1A - cur) < 0 && !(TIFR1 & (1<<OCF1A)))
        // Already processing timer irqs
        try_shutdown("timer_set_next called during timer dispatch");
    uint32_t mintime = calc_time(timer_last, cur + TIMER_MIN_TICKS);
    if (sched_is_before(mintime, next)) {
        timer_set_clear(next);
        return 0;
    }
    timer_set_clear(mintime);
    return 1;
}

static uint8_t timer_repeat;
#define TIMER_MAX_REPEAT 40
#define TIMER_MAX_NEXT_REPEAT 15

#define TIMER_MIN_TRY_TICKS 60 // 40 ticks to exit irq; 20 ticks of progress
#define TIMER_DEFER_REPEAT_TICKS 200

// Similar to timer_set_next(), but wait for the given time if it is
// in the near future.
uint8_t
timer_try_set_next(uint32_t target)
{
    uint16_t next = target, now = timer_get();
    int16_t diff = next - now;
    if (diff > TIMER_MIN_TRY_TICKS)
        // Schedule next timer normally.
        goto done;

    // Next timer is in the past or near future - can't reschedule to it
    uint8_t tr = timer_repeat-1;
    if (likely(tr)) {
        irq_enable();
        timer_repeat = tr;
        irq_disable();
        while (diff >= 0) {
            // Next timer is in the near future - wait for time to occur
            now = timer_get();
            irq_enable();
            diff = next - now;
            irq_disable();
        }
        return 0;
    }

    // Too many repeat timers from a single interrupt - force a pause
    timer_repeat = TIMER_MAX_NEXT_REPEAT;
    next = now + TIMER_DEFER_REPEAT_TICKS;
    if (diff < (int16_t)(-timer_from_us(1000)))
        goto fail;

done:
    timer_set(next);
    return 1;
fail:
    shutdown("Rescheduled timer in the past");
}

static void
timer_task(void)
{
    timer_repeat = TIMER_MAX_REPEAT;
}
DECL_TASK(timer_task);
