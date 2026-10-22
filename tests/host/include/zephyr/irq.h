#ifndef HOST_ZEPHYR_IRQ_H
#define HOST_ZEPHYR_IRQ_H
#define USBD_IRQn 3
#define ISR_DIRECT_DECLARE(name) int name(void)
#define IRQ_DIRECT_CONNECT(...) ((void)0)
static inline void irq_enable(int irq) { (void)irq; }
#endif
