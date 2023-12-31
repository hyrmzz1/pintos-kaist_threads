#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */
// TIMER_FREQ 최소값 및 최소값 설정
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
// 짧은 지연을 구현하는 데 사용되는 loops_per_tick을 보정(조정)
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;	// ticks(OS 부팅 이후 타이머 틱 수)의 현재값 저장. 계속 올라감.
	intr_set_level (old_level);	// old_level에 저장된 이전 인터럽트 상태 복원
	barrier ();
	return t;	// 현재 타이머 틱 수 반환
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {	// 특정 시점 이후 경과된 타이머 틱 수 계산
	return timer_ticks () - then;	// 현재 타이머 틱 수 - 이전 타이머 틱 수. (경과된 시간을 타이머 틱 단위로 나타냄)
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {	// ticks만큼 재운다
	/* busy waiting 방식 */
	/*
	int64_t start = timer_ticks ();	// 함수 호출될 때 현재 타이머 틱 수 기록 (= 시작, 현재 시간)

	ASSERT (intr_get_level () == INTR_ON);	// 인터럽트 활성화되어있는지 확인. ASSERT(): 조건식이 true일 때 프로그램 계속 실행, false이면 프로그램 중단.
	while (timer_elapsed (start) < ticks)	// 경과 시간이 지정된 ticks보다 작으면 아직 깨울 시간 안됐다는 것.
		thread_yield ();	// running status => ready status. CPU 양도, 다른 스레드 실행되도록 함.
	*/

	/* sleep-wakeup 방식 (thread_yield() & do_schedule() 대체) */
	int64_t start = timer_ticks ();	// 함수 호출될 때 현재 타이머 틱 수(= 시작, 현재 시간) 기록.
	thread_sleep(start + ticks);	// 깨울 시간을 parameter로
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {	// 매 tick마다 해당 tick에 깨워야 할 쓰레드들 깨우기
	ticks++;	// 시간 계속 흐르니까 틱도 계속 증가.
	thread_tick ();	// ticks => 깨울 시간

	if (thread_mlfqs){
		mlfqs_increment();	// timer_interrupt 발생할 때마다(= 틱 증가할 때마다, 위에 정의되어 있음) recuent_cpu 1 증가

		/* 매 4tick마다 현재 실행 중인 쓰레드의 priority 계산 => 모든 프로세스의 우선순위 다시 계산하는거 아닌가 ???*/
		if (timer_ticks() % 4 == 0){	// 왜 thread_ticks 아니고 timer_ticks() 사용하지?
			mlfqs_priority(thread_current());
			// 1초마다 모든 프로세스의 recent_cpu 업데이트
			// 근데 왜 여기선 현재 쓰레드만의 load_avg, recent_cpu 계산??
			if (timer_ticks() % TIMER_FREQ == 0){
				mlfqs_load_avg();
				// mlfqs_recent_cpu(thread_current());
				mlfqs_recalc();	// why..........
			}
		}
	}
	thread_awake (ticks);	// ticks에 깨운다
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks; 
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
