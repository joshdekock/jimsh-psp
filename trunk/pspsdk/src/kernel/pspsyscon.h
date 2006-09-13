/*
 * PSP Software Development Kit - http://www.pspdev.org
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPSDK root for details.
 *
 * pspsyscon.h - Interface to sceSysreg_driver.
 *
 * Copyright (c) 2006 James F
 *
 * $Id$
 */

#ifndef PSPSYSCON_H
#define PSPSYSCON_H

#include <pspkerneltypes.h>

/** @defgroup Syscon Interface to the sceSyscon_driver library.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup Syscon Interface to the sceSyscon_driver library. */
/*@{*/

/**
  * Force the PSP to go into standby
  */
void sceSysconPowerStandby(void);

/**
 * Reset the PSP
 *
 * @param unk1 - Unknown, pass 1
 * @param unk2 - Unknown, pass 1
 */
void sceSysconResetDevice(int unk1, int unk2);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif