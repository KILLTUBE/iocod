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
#define MAX_DYNAMIC_PATCH_INSTANCES 128
typedef struct {
  patchCollide_t *pc;  // pointer to static patchCollide
  vec3_t origin;        // world position
} dynamicPatchInstance_t;
extern dynamicPatchInstance_t dynamicPatchInstances[MAX_DYNAMIC_PATCH_INSTANCES];
extern int numDynamicPatchInstances;
dynamicPatchInstance_t *CM_AddDynamicPatchInstance(patchCollide_t *pc, const vec3_t origin);
#endif
