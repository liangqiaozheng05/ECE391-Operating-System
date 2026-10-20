/*! @file ramdisk.h‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‍‍‍‌‌‍‌​⁠⁠‌
    @brief Memory-backed storage interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifndef _RAMDISK_H_
#define _RAMDISK_H_

#include <stddef.h>

struct storage;  // forward declaration

// EXPORTED FUNCTION DECLARATIONS
//

extern void ramdisk_attach();
// static int ramdisk_open(struct storage *sto);
// static void ramdisk_close(struct storage *sto);
// static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf, unsigned long bytecnt);
// static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

#endif  // _RAMDISK_H_