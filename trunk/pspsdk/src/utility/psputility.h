/*
 * PSP Software Development Kit - http://www.pspdev.org
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPSDK root for details.
 *
 *  psputility.h - Master include for the pspUtility library
 *
 * Copyright (c) 2005 John Kelley <ps2dev@kelley.ca>
 *
 * $Id$
 */
#ifndef __PSPUTILITY_H__
#define __PSPUTILITY_H__

typedef struct
{
	unsigned int size;	/** Size of the structure */
	int language;		/** Language */
	int buttonSwap;		/** Set to 1 for X/O button swap */
	int graphicsThread;	/** Graphics thread priority */
	int unknown;		/** Some other thread priority? */
	int fontThread;		/** Font (?) thread priority (ScePafThread) */
	int soundThread;	/** Sound thread priority */
	int result;             /** Result */
	int unknown2[4];

} pspUtilityDialogCommon;

#include <psputility_msgdialog.h>
#include <psputility_netconf.h>
#include <psputility_netparam.h>
#include <psputility_savedata.h>
#include <psputility_sysparam.h>
#include <psputility_osk.h>
#include <psputility_netmodules.h>

#define PSP_UTILITY_ACCEPT_CIRCLE 0
#define PSP_UTILITY_ACCEPT_CROSS  1

/**
 * Return-values for the various sceUtility***GetStatus() functions
**/
typedef enum
{
	PSP_UTILITY_DIALOG_NONE = 0,	/**< No dialog is currently active */
	PSP_UTILITY_DIALOG_INIT,	/**< The dialog is currently being initialized */
	PSP_UTILITY_DIALOG_VISIBLE,	/**< The dialog is visible and ready for use */
	PSP_UTILITY_DIALOG_QUIT,	/**< The dialog has been canceled and should be shut down */
	PSP_UTILITY_DIALOG_FINISHED	/**< The dialog has successfully shut down */
	
} pspUtilityDialogState;

#endif
