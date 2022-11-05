/**
 * OpenMfx modifier for Blender
 * Copyright (C) 2019 - 2022 Elie Michel
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
 * \ingroup openmfx
 */

#include <OpenMfx/Sdk/Cpp/Host/Host>
#include <OpenMfx/Sdk/Cpp/Host/EffectLibrary>

#include "ofxCore.h"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include <map>
#include <string>

class BlenderMfxHost;

namespace blender::modifiers::modifier_open_mfx_cc {

/**
 * Runtime data for the OpenMfx MODIFIER. For the Geometry node, see MFX_node_runtime.h
 * Structure holding runtime allocated data for OpenMfx plug-in hosting.
 * It ensures communication between Blender's RNA (OpenMfxModifierData)
 * and the non-GPL mfxHost.
 */
class RuntimeData {
 public:
  RuntimeData();
  ~RuntimeData();

  /**
   * Set the plugin path, as put return status in is_plugin_valid
   */
  void set_plugin_path(const char *plugin_path);

  /**
   * Pick an effect in the plugin by its index. Value is clamped to valid values (including -1 to
   * mean "no effect selected").
   */
  void set_effect_index(int effect_index);

  /**
   * Set parameter values from Blender's RNA to the Open Mesh Effect host's structure.
   */
  void get_parameters_from_rna(OpenMfxModifierData *fxmd);

  /**
   * Copy messages returned by the plugin in the RNA
   */
  void set_message_in_rna(OpenMfxModifierData *fxmd);

  /**
   * Ensures that the effect descriptor and instances are valid (may fail, and hence return false)
   */
  bool ensure_effect_instance();

  /**
   * Tells whether the plugin specified by plugin_path is valid. If true, then 'registry' can be
   * used
   */
  bool is_plugin_valid() const;

  /**
   * Cache current value of the parameters. This is used to try to remember these parameters while
   * reloading plugins.
   */
  void save_rna_parameter_values(OpenMfxModifierData *fxmd);

  /**
   * Restore the cache current value of the parameters, only if parameter names match.
   */
  void try_restore_rna_parameter_values(OpenMfxModifierData *fxmd);

  /**
   * Actually apply the modifier
   */
  Mesh *cook(OpenMfxModifierData *fxmd, const Depsgraph *depsgraph, Mesh *mesh, Object *object);

  /**
   * Reload the list of effects contaiend in the plugin
   */
  void reload_effect_info(OpenMfxModifierData *fxmd);

  /**
   * Reload the list of parameters of the current effect
   */
  void reload_parameters(OpenMfxModifierData *fxmd);

  /**
   * Reload the list of extra inputs of the current effect
   */
  void reload_extra_inputs(OpenMfxModifierData *fxmd);

  /**
   * Update input properties (e.g. request_transform) from the current effect descriptor
   */
  void set_input_prop_in_rna(OpenMfxModifierData *fxmd);

 public:
  /**
   * Path to the OFX plug-in bundle.
   */
  char plugin_path[1024];

  /**
   * Plug-in registry (as defined in mfxHost.h) holding the list of available filters within the
   * OFX bundle. This is a pointer to a reference-counted registry handled by mfxPluginRegistryPool
   */
  OpenMfx::EffectLibrary *library;

  /**
   * Index of the current effect within the list of effects contained in the currently opened
   * plugin. A value of -1 means none.
   */
  int effect_index;


  // OFX data handles
  
  /**
   * Pointer to the BlenderMfxHost singleton.
   */
  BlenderMfxHost *mfx_host;

  /**
   * Descriptor of the effect, listing its parameters for instance
   */
  OfxMeshEffectHandle effect_desc;

  /**
   * Effect instance, used for cooking
   */
  OfxMeshEffectHandle effect_instance;

private:
  /**
   * Free the descriptor and instance, if it had been allocated (otherwise does nothing)
   */
  void free_effect_instance();

  /**
   * Ensures that the ofx_host member if a valid OfxHost
   */
  void ensure_host();

  /**
   * Ensures that the plugin path is unloaded and reset
   */
  void reset_plugin_path();

private:
  /**
   * Tells whether the plugin specified by plugin_path is valid. If true, then 'registry' can be
   * used
   */
  bool m_is_plugin_valid;

  std::map<std::string, OfxParamStruct> m_saved_parameter_values;
};

}  // namespace blender::modifiers::modifier_open_mfx_cc
