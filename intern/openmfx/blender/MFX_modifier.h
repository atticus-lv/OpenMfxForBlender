/**
 * Open Mesh Effect modifier for Blender
 * Copyright (C) 2019 Elie Michel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/** \file
 * \ingroup openmesheffect
 *
 * This is an implementation of an OpenFX host specialized toward the Mesh
 * Effect API (rather than the Image Effect API like most OpenFX host
 * implementations are.)
 */

#ifndef __MFX_MODIFIER_H__
#define __MFX_MODIFIER_H__

/**
 * This is called from C code and handles the connection with the C++
 * based implementation of the modifier (mfxRuntime) that is stored in
 * the 'runtime' field of the ModifierData.
 */

#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_mesh_types.h"

#ifndef RNA_RUNTIME
#  include "MOD_modifiertypes.h"
#endif

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#ifdef __cplusplus
extern "C" {
#endif

// A C-enum version of OpenMfx::ParameterType
enum MfxParamType {
  PARAM_TYPE_UNKNOWN = -1,
  PARAM_TYPE_INTEGER,
  PARAM_TYPE_INTEGER_2D,
  PARAM_TYPE_INTEGER_3D,
  PARAM_TYPE_DOUBLE,
  PARAM_TYPE_DOUBLE_2D,
  PARAM_TYPE_DOUBLE_3D,
  PARAM_TYPE_RGB,
  PARAM_TYPE_RGBA,
  PARAM_TYPE_BOOLEAN,
  PARAM_TYPE_CHOICE,
  PARAM_TYPE_STRING,
  PARAM_TYPE_CUSTOM,
  PARAM_TYPE_PUSHBUTTON,
  PARAM_TYPE_GROUP,
  PARAM_TYPE_PAGE,
};


// Callback types required by mfxHost
typedef Mesh* (*MeshNewFunc)(int, int, int, int, int); // BKE_mesh_new_nomain
typedef void (*MeshPostFunc)(Mesh*); // post processing applied to new mesh

/**
 * Copy from runtime data (runtime_data->registry) to RNA (fxmd->effect_info)
 * the meta information about effects contained in the bundle.
 * Populate fxmd->num_effects and fxmd->effect_info unless runtime_data->is_plugin_valid if false
 */
void MFX_modifier_reload_effect_info(OpenMfxModifierData *fxmd);

/**
 * Called when the "plugin path" field is changed.
 * It completes runtime_set_plugin_path by updating DNA data (fxmd).
 */
void MFX_modifier_on_plugin_changed(OpenMfxModifierData *fxmd);

/**
 * Called when the user switched to another effect within the same plugin bundle.
 */
void MFX_modifier_on_effect_changed(OpenMfxModifierData *fxmd);

void MFX_modifier_free_runtime_data(void *runtime_data);

/**
 * Actually run the modifier, calling the cook action of the plugin
 */
Mesh *MFX_modifier_do(OpenMfxModifierData *fxmd,
                      const Depsgraph *depsgraph,
                      Mesh *mesh,
                      Object *object);

/**
 * Copy parameter_info, effect_info.
 * Must be called *after* blender's modifier_copyData_generic()
 */
void MFX_modifier_copydata(OpenMfxModifierData *source,
                           OpenMfxModifierData *destination);

void MFX_modifier_before_update_depsgraph(OpenMfxModifierData *fxmd);

#ifdef __cplusplus
}
#endif

#endif // __MFX_MODIFIER_H__
