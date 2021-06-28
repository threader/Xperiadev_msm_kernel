/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2013 ARM Limited
 */

#ifndef __ASM_PSCI_H
#define __ASM_PSCI_H

int psci_init(void);



extern struct psci_operations psci_ops;
#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
int psci_apply_bp_hardening(void);
#endif /* CONFIG_HARDEN_BRANCH_PREDICTOR */

#endif /* __ASM_PSCI_H */
