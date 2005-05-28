/* kernel.h */

#ifndef __KERNEL_H__
#define __KERNEL_H__

/** @defgroup Kernel PSP Kernel Import Library */

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup Kernel */
/*@{*/

/** Define the elf name variable, contains a pointer to the boot elf path */
extern const char* g_elf_name;

/** Define the elf name size variable */
extern int g_elf_namesize;

/*@}*/

#include <stddef.h>
#include <stdarg.h>

#include <tamtypes.h>
#include <ctrl.h>
#include <display.h>
#include <fileio.h>
#include <loadexec.h>
#include <modload.h>
#include <threadman.h>
#include <utils.h>
#include <umd.h>
#include <ge.h>

#ifdef __cplusplus
}
#endif

#endif
