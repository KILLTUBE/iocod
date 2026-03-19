#ifndef __CM_DYNAMIC_PATCHES__
#define __CM_DYNAMIC_PATCHES__
#include "q_shared.h"
#include "qcommon.h"
#include "cm_patch.h"
#define MAX_DYNAMIC_PATCHES 128
// store the patches directly
extern patchCollide_t dynamicPatchCollides[MAX_DYNAMIC_PATCHES];
extern int numDynamicPatches;
int CM_AddDynamicPatch(patchCollide_t *pc);
#endif
