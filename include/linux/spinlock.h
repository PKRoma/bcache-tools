#ifndef __TOOLS_LINUX_SPINLOCK_H
#define __TOOLS_LINUX_SPINLOCK_H

#include <linux/atomic.h>

typedef struct {
	int		count;
} raw_spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED(name)	(raw_spinlock_t) { .count = 0 }

static inline void raw_spin_lock_init(raw_spinlock_t *lock)
{
	smp_store_release(&lock->count, 0);
}

static inline void raw_spin_lock(raw_spinlock_t *lock)
{
	while (xchg_acquire(&lock->count, 1))
		;
}

static inline void raw_spin_unlock(raw_spinlock_t *lock)
{
	smp_store_release(&lock->count, 0);
}

#define raw_spin_lock_irq(lock)		raw_spin_lock(lock)
#define raw_spin_unlock_irq(lock)	raw_spin_unlock(lock)

#define raw_spin_lock_irqsave(lock, flags)		\
do {							\
	(void) flags;					\
	raw_spin_lock(lock);				\
} while (0)

#define raw_spin_unlock_irqrestore(lock, flags) raw_spin_unlock(lock)

typedef raw_spinlock_t spinlock_t;

#define __SPIN_LOCK_UNLOCKED(name)	__RAW_SPIN_LOCK_UNLOCKED(name)

#define DEFINE_SPINLOCK(x)	spinlock_t x = __SPIN_LOCK_UNLOCKED(x)

#define spin_lock_init(lock)		raw_spin_lock_init(lock)
#define spin_lock(lock)			raw_spin_lock(lock)
#define spin_unlock(lock)		raw_spin_unlock(lock)

#define spin_lock_nested(lock, n)	spin_lock(lock)

#define spin_lock_bh(lock)		raw_spin_lock(lock)
#define spin_unlock_bh(lock)		raw_spin_unlock(lock)

#define spin_lock_irq(lock)		raw_spin_lock(lock)
#define spin_unlock_irq(lock)		raw_spin_unlock(lock)

#define spin_lock_irqsave(lock, flags)	raw_spin_lock_irqsave(lock, flags)
#define spin_unlock_irqrestore(lock, flags) raw_spin_unlock_irqrestore(lock, flags)

#endif /* __TOOLS_LINUX_SPINLOCK_H */
