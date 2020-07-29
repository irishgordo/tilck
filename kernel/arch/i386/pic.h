/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>

void init_pic_8259(u8 offset1, u8 offset2);
void pic_send_eoi(int irq);
bool pic_is_spur_irq(int irq);
u16 pic_get_irr(void);
u16 pic_get_isr(void);
void irq_set_mask(int irq);
void irq_clear_mask(int irq);