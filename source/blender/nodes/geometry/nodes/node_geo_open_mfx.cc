/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright (c) 2022 - Elie Michel
 */

#include "BLI_disjoint_set.hh"
#include "BLI_task.hh"
#include "BLI_vector_set.hh"
#include "BLI_math_vec_types.hh"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_socket.h"

#include "MFX_node_runtime.h"

#include "node_geometry_util.hh"

// XXX We use an internal header of bf_intern_openmfx, either turn this to an external header or
// get the host from node_runtime.
#include "intern/BlenderMfxHost.h"
#include "TinyTimer.h"

#include <OpenMfx/Sdk/Cpp/Host/MeshEffect>
#include <OpenMfx/Sdk/Cpp/Host/Properties>

using MeshInternalDataNode = BlenderMfxHost::MeshInternalDataNode;
using OpenMfx::AttributeProps;

namespace blender::nodes::node_geo_open_mfx_cc {

// -----------------------------------------
// Utils

static const char *MFX_input_label(const OfxMeshInputStruct &input)
{
  // This label must be unique because Blender uses the same string
  // both for display and for identification
  // XXX Should be return input.name().c_str() in any case then?
  // OpenMfx does not ensure that labels are unique, but it ensures
  // that names are, but on the other hand names are not meant for
  // UI display and labels are unlikely to have duplicates.
  int labelIndex = input.properties.find(kOfxPropLabel);
  return labelIndex >= 0 ? input.properties[labelIndex].value[0].as_const_char : input.name().c_str();
}

static const char *MFX_param_label(const OfxParamStruct &param)
{
  // Same as for MFX_input_label
  int labelIndex = param.properties.find(kOfxPropLabel);
  return labelIndex >= 0 ? param.properties[labelIndex].value[0].as_const_char :
                           param.name;
}

NodeWarningType MFX_message_type(OfxMessageType messageType)
{
  switch (messageType) {
    case OfxMessageType::Warning:
    case OfxMessageType::Invalid:
      return NodeWarningType::Warning;
    case OfxMessageType::Error:
    case OfxMessageType::Fatal:
      return NodeWarningType::Error;
    case OfxMessageType::Log:
    case OfxMessageType::Message:
    case OfxMessageType::Question:
    default:
      return NodeWarningType::Info;
  }
}

static int MFX_element_count(const GeometryComponent &component,
                             OpenMfx::AttributeAttachment attachment)
{
  switch (attachment) {
    case OfxAttributeStruct::AttributeAttachment::Point:
      return component.attribute_domain_size(ATTR_DOMAIN_POINT);
    case OfxAttributeStruct::AttributeAttachment::Corner:
      return component.attribute_domain_size(ATTR_DOMAIN_CORNER);
    case OfxAttributeStruct::AttributeAttachment::Face:
      return component.attribute_domain_size(ATTR_DOMAIN_FACE);
    case OfxAttributeStruct::AttributeAttachment::Mesh:
      return 1;
    default:
      BLI_assert(false);
      return 0;
  }
}

static void MFX_node_add_attrib_input(NodeDeclarationBuilder &b, const OfxAttributeStruct &attrib)
{
  int componentCount = attrib.componentCount();
  OpenMfx::AttributeSemantic semantic = attrib.semantic();
  const std::string &name = attrib.name();

  switch (attrib.type()) {
    case OpenMfx::AttributeType::Float:
      if (semantic == OpenMfx::AttributeSemantic::Color) {
        b.add_input<decl::Color>(name).supports_field();
      }
      else if (componentCount == 1) {
        b.add_input<decl::Float>(name).supports_field();
      }
      else if (componentCount <= 3) {
        b.add_input<decl::Vector>(name).supports_field();
      }
      else
      {
        BLI_assert(false);  // Unsupported combination
      }
      break;
    case OpenMfx::AttributeType::Int:
      if (componentCount == 1) {
        b.add_input<decl::Int>(name).supports_field();
      }
      else {
        BLI_assert(false); // Unsupported combination
      }
      break;
    case OpenMfx::AttributeType::UByte:
      BLI_assert(false);  // Unsupported combination
      break;
  }
}

static void MFX_node_add_attrib_output(NodeDeclarationBuilder &b, const OfxAttributeStruct &attrib)
{
  int componentCount = attrib.componentCount();
  OpenMfx::AttributeSemantic semantic = attrib.semantic();
  const std::string &name = attrib.name();

  switch (attrib.type()) {
    case OpenMfx::AttributeType::Float:
      if (semantic == OpenMfx::AttributeSemantic::Color) {
        b.add_output<decl::Color>(name).field_source();
      }
      else if (componentCount == 1) {
        b.add_output<decl::Float>(name).field_source();
      }
      else if (componentCount <= 3) {
        b.add_output<decl::Vector>(name).field_source();
      }
      else {
        BLI_assert(false);  // Unsupported combination
      }
      break;
    case OpenMfx::AttributeType::Int:
      if (componentCount == 1) {
        b.add_output<decl::Int>(name).field_source();
      }
      else {
        BLI_assert(false);  // Unsupported combination
      }
      break;
    case OpenMfx::AttributeType::UByte:
      BLI_assert(false);  // Unsupported combination
      break;
  }
}

static void MFX_node_add_geo_input(NodeDeclarationBuilder &b,
                                   const OfxMeshInputStruct &input)
{
  const char *label = MFX_input_label(input);
  if (kOfxMeshMainOutput == input.name()) {
    b.add_output<decl::Geometry>(label);
    int n = input.requested_attributes.count();
    for (int i = 0; i < n; ++i) {
      MFX_node_add_attrib_output(b, input.requested_attributes[i]);
    }
  }
  else {
    b.add_input<decl::Geometry>(label);
    int n = input.requested_attributes.count();
    for (int i = 0; i < n; ++i) {
      MFX_node_add_attrib_input(b, input.requested_attributes[i]);
    }
  }
}

static void MFX_node_add_param_input(NodeDeclarationBuilder &b,
                                     const OfxParamStruct &param)
{
  const char *label = MFX_param_label(param);
  switch (param.type) {
    case OpenMfx::ParameterType::Integer:
      b.add_input<decl::Int>(label);
      break;
    case OpenMfx::ParameterType::Integer2d:
      b.add_input<decl::Int>((std::string(label) + ".x").c_str());
      b.add_input<decl::Int>((std::string(label) + ".y").c_str());
      break;
    case OpenMfx::ParameterType::Integer3d:
      b.add_input<decl::Int>((std::string(label) + ".x").c_str());
      b.add_input<decl::Int>((std::string(label) + ".y").c_str());
      b.add_input<decl::Int>((std::string(label) + ".z").c_str());
      break;
    case OpenMfx::ParameterType::Double:
      b.add_input<decl::Float>(label);
      break;
    case OpenMfx::ParameterType::Double2d:
      b.add_input<decl::Vector>(label);
      break;
    case OpenMfx::ParameterType::Double3d:
      b.add_input<decl::Vector>(label);
      break;
    case OpenMfx::ParameterType::Rgb:
      b.add_input<decl::Color>(label);
      break;
    case OpenMfx::ParameterType::Rgba:
      b.add_input<decl::Color>(label);
      break;
    case OpenMfx::ParameterType::Boolean:
      b.add_input<decl::Bool>(label);
      break;
    case OpenMfx::ParameterType::Choice:
      b.add_input<decl::Int>(label);
      break;
    case OpenMfx::ParameterType::String:
      b.add_input<decl::String>(label);
      break;
    case OpenMfx::ParameterType::Custom:
    case OpenMfx::ParameterType::PushButton:
    case OpenMfx::ParameterType::Group:
    case OpenMfx::ParameterType::Page:
    case OpenMfx::ParameterType::Unknown:
    default:
      break;
  }
}

static void MFX_node_extract_param(GeoNodeExecParams &b, OfxParamStruct &param)
{
  const char *label = MFX_param_label(param);
  switch (param.type) {
    case OpenMfx::ParameterType::Integer:
    case OpenMfx::ParameterType::Choice:
      param.value[0].as_int = b.extract_input<int>(label);
      break;
    case OpenMfx::ParameterType::Integer2d:
      param.value[0].as_int = b.extract_input<int>((std::string(label) + ".x").c_str());
      param.value[1].as_int = b.extract_input<int>((std::string(label) + ".y").c_str());
      break;
    case OpenMfx::ParameterType::Integer3d:
      param.value[0].as_int = b.extract_input<int>((std::string(label) + ".x").c_str());
      param.value[1].as_int = b.extract_input<int>((std::string(label) + ".y").c_str());
      param.value[2].as_int = b.extract_input<int>((std::string(label) + ".z").c_str());
      break;
    case OpenMfx::ParameterType::Double:
      param.value[0].as_double = b.extract_input<float>(label);
      break;
    case OpenMfx::ParameterType::Double2d: {
      float2 value = b.extract_input<float2>(label);
      param.value[0].as_double = value[0];
      param.value[1].as_double = value[1];
      break;
    }
    case OpenMfx::ParameterType::Double3d:
    case OpenMfx::ParameterType::Rgb: {
      float3 value = b.extract_input<float3>(label);
      param.value[0].as_double = value[0];
      param.value[1].as_double = value[1];
      param.value[2].as_double = value[2];
      break;
    }
    case OpenMfx::ParameterType::Rgba: {
      ColorGeometry4f value = b.extract_input<ColorGeometry4f>(label);
      param.value[0].as_double = value[0];
      param.value[1].as_double = value[1];
      param.value[2].as_double = value[2];
      param.value[3].as_double = value[3];
      break;
    }
    case OpenMfx::ParameterType::Boolean:
      param.value[0].as_bool = b.extract_input<bool>(label);
      break;
    case OpenMfx::ParameterType::String:
      param.value[0].as_const_char = b.extract_input<std::string>(label).c_str();
      break;
    case OpenMfx::ParameterType::Custom:
    case OpenMfx::ParameterType::PushButton:
    case OpenMfx::ParameterType::Group:
    case OpenMfx::ParameterType::Page:
    case OpenMfx::ParameterType::Unknown:
    default:
      break;
  }
}

static const CPPType &MFX_to_cpptype(OpenMfx::AttributeType mfxType, int componentCount)
{
  // TODO: use componentCount
  //(void)componentCount;
  switch (mfxType) {
    case OfxAttributeStruct::AttributeType::Float:
      if (componentCount == 1)
        return CPPType::get<float>();
      else if (componentCount <= 3)
        return CPPType::get<float3>();
      else {
        BLI_assert(false);  // not implemented
        return CPPType::get<int>();
      }
    case OfxAttributeStruct::AttributeType::Int:
      return CPPType::get<int>();
    default:
      BLI_assert(false);  // not implemented
      return CPPType::get<float>();
  }
}

static void MFX_node_set_message(GeoNodeExecParams &params, OfxMeshEffectHandle effect)
{
  if (effect->message != nullptr && effect->message[0] != '\0') {
    NodeWarningType messageType = MFX_message_type(effect->messageType);
    std::string message(effect->message);
    params.error_message_add(messageType, message);
  }
  else {
    params.error_message_add(NodeWarningType::Error, TIP_("Failed to cook effect"));
  }
}

// Try to setup runtime data and return true if it
// succeeds or if it was already defined.
static bool MFX_node_try_ensure_runtime(bNode *node)
{
  if (node == nullptr || node->storage == nullptr) {
    return false;
  }

  NodeGeometryOpenMfx &storage = *((NodeGeometryOpenMfx *)node->storage);

  if (nullptr == storage.runtime) {
    storage.runtime = MEM_new<RuntimeData>(__func__);
    storage.runtime->setPluginPath(storage.plugin_path);
    storage.runtime->setEffectIndex(storage.effect_index);
  }

  return true;
}

// TODO: use templates instead
static void copy_based_on_IOMap(Span<float> srcData,
                                MutableSpan<float> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  int originPoint = 0;
  for (const int i : destData.index_range()) {
    int nbOriginPoints = getOriginPointsPoolSize(i);
    destData[i] = 0.0f;
    for (int l = 0; l < nbOriginPoints; l++) {
      int originPointIndex = getOriginPointIndex(originPoint);
      float originPointWeight = getOriginPointWeight(originPointIndex);
      destData[i] += srcData[originPointIndex] * originPointWeight;
      originPoint++;
    }
  }
}

static void copy_based_on_IOMap(Span<bool> srcData,
                                MutableSpan<bool> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  int originPoint = 0;
  for (const int i : destData.index_range()) {
    int nbOriginPoints = getOriginPointsPoolSize(i);
    float estimation = 0.0f;
    for (int l = 0; l < nbOriginPoints; l++) {
      int originPointIndex = getOriginPointIndex(originPoint);
      float originPointWeight = getOriginPointWeight(originPointIndex);
      estimation += srcData[originPointIndex] * originPointWeight;
      originPoint++;
    }
    destData[i] = estimation > 0.5f; //
  }
}

static void copy_based_on_IOMap(Span<int> srcData,
                                MutableSpan<int> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  int originPoint = 0;
  for (const int i : destData.index_range()) {
    int nbOriginPoints = getOriginPointsPoolSize(i);
    float estimation = 0.0f;
    for (int l = 0; l < nbOriginPoints; l++) {
      int originPointIndex = getOriginPointIndex(originPoint);
      float originPointWeight = getOriginPointWeight(originPointIndex);
      estimation += srcData[originPointIndex] * originPointWeight;
      originPoint++;
    }
    destData[i] = static_cast<int>(estimation);  //
  }
}

static void copy_based_on_IOMap(Span<float2> srcData,
                                MutableSpan<float2> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  int originPoint = 0;
  for (const int i : destData.index_range()) {
    int nbOriginPoints = getOriginPointsPoolSize(i);
    destData[i] = {};
    for (int l = 0; l < nbOriginPoints; l++) {
      int originPointIndex = getOriginPointIndex(originPoint);
      float originPointWeight = getOriginPointWeight(originPointIndex);
      destData[i] += srcData[originPointIndex] * originPointWeight;
      originPoint++;
    }
  }
}

static void copy_based_on_IOMap(Span<float3> srcData,
                                MutableSpan<float3> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  int originPoint = 0;
  for (const int i : destData.index_range()) {
    int nbOriginPoints = getOriginPointsPoolSize(i);
    destData[i] = {};
    for (int l = 0; l < nbOriginPoints; l++) {
      int originPointIndex = getOriginPointIndex(originPoint);
      float originPointWeight = getOriginPointWeight(originPointIndex);
      destData[i] += srcData[originPointIndex] * originPointWeight;
      originPoint++;
    }
  }
}

template<typename T>
static void copy_based_on_IOMap(Span<T> srcData,
                                MutableSpan<T> destData,
                                std::function<int(int)> getOriginPointsPoolSize,
                                std::function<int(int)> getOriginPointIndex,
                                std::function<float(int)> getOriginPointWeight)
{
  // TODO: not handled
  return;
}

static void propagate_attributes(const Map<AttributeIDRef, AttributeKind> &attributes,
                                 const bke::AttributeAccessor src_attributes,
                                 bke::MutableAttributeAccessor dst_attributes,
                                 const Span<eAttrDomain> domains,
                                 const Span<eCustomDataType> types,
                                 std::function<int(int)> getOriginPointsPoolSize,
                                 std::function<int(int)> getOriginPointIndex,
                                 std::function<float(int)> getOriginPointWeight)
{
  for (Map<AttributeIDRef, AttributeKind>::Item entry : attributes.items()) {
    const AttributeIDRef attribute_id = entry.key;
    GAttributeReader attribute = src_attributes.lookup(attribute_id);
    if (!attribute) {
      continue;
    }

    const eCustomDataType data_type = bke::cpp_type_to_custom_data_type(attribute.varray.type());

    /* Only copy if it is on a domain and of a type we want. */
    if (!domains.contains(attribute.domain) || !types.contains(data_type)) {
      continue;
    }

    GSpanAttributeWriter result_attribute = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, attribute.domain, data_type);

    if (!result_attribute) {
      continue;
    }

    attribute_math::convert_to_static_type(data_type, [&](auto dummy) {
      using T = decltype(dummy);
      VArraySpan<T> span{attribute.varray.typed<T>()};
      MutableSpan<T> out_span = result_attribute.span.typed<T>();
      copy_based_on_IOMap(
          span, out_span, getOriginPointsPoolSize, getOriginPointIndex, getOriginPointWeight); 
    });
    result_attribute.finish();
  }
}

// -----------------------------------------
// Node Callbacks

NODE_STORAGE_FUNCS(NodeGeometryOpenMfx)

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node();
  if (node == nullptr || node->storage == nullptr) {
    return;
  }

  const NodeGeometryOpenMfx &storage = node_storage(*node);

  if (storage.runtime != nullptr) {
    OfxMeshEffectHandle desc = storage.runtime->effectDescriptor();
    if (desc != nullptr) {
      const OfxMeshInputSetStruct &inputs = desc->inputs;
      for (int i = 0; i < inputs.count(); ++i) {
        MFX_node_add_geo_input(b, inputs[i]);
      }
      const OfxParamSetStruct &params = desc->parameters;
      for (int i = 0; i < params.count(); ++i) {
        MFX_node_add_param_input(b, params[i]);
      }
    }
  }
  #if 0
  else {
    b.add_input<decl::Float>(N_("Radius"))
        .default_value(1.0f)
        .min(0.0f)
        .subtype(PROP_DISTANCE)
        .description(N_("Size of the pizza"));
    b.add_output<decl::Geometry>("Mesh");
    b.add_output<decl::Bool>(N_("Base")).field_source();
    b.add_output<decl::Bool>(N_("Olives")).field_source();
  }
  #endif
}

static void node_layout(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  uiItemR(layout, ptr, "plugin_path", 0, "", ICON_NONE);
  uiItemR(layout, ptr, "effect_enum", 0, "", ICON_NONE);
}

static void node_init(bNodeTree *UNUSED(tree), bNode *node)
{
  NodeGeometryOpenMfx *data = MEM_cnew<NodeGeometryOpenMfx>(__func__);
  data->runtime = MEM_new<RuntimeData>(__func__);
  // elie: or node->runtime now that it's been added?
  node->storage = data;
}

/* We use custom free and copy function because of manually allocated runtime data */
static void node_free_storage(struct bNode *node)
{
  NodeGeometryOpenMfx &storage = node_storage(*node);
  if (storage.runtime != nullptr) {
    MEM_delete<RuntimeData>(storage.runtime);
    storage.runtime = nullptr;
  }
  node_free_standard_storage(node);
}

static void node_copy_storage(struct bNodeTree *dest_ntree,
                       struct bNode *dest_node,
                       const struct bNode *src_node)
{
  node_copy_standard_storage(dest_ntree, dest_node, src_node);
  NodeGeometryOpenMfx &dest_storage = node_storage(*dest_node);
  const NodeGeometryOpenMfx &src_storage = node_storage(*src_node);
  dest_storage.runtime = MEM_new<RuntimeData>(__func__);
  *dest_storage.runtime = *src_storage.runtime;
}

/**
 * Trigger from node_update a new call to node_declare (but we do not call
 * node_declare directly so that internal node function take care of
 * boilerplate like updating UI etc.)
 */
static void force_redeclare(bNodeTree *ntree, bNode *node)
{
  BLI_assert(node->typeinfo->declaration_is_dynamic);
  if (node->runtime->declaration != nullptr)
  {
    delete node->runtime->declaration;
    node->runtime->declaration = nullptr;
  }
  node_verify_sockets(ntree, node, true);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  if (!MFX_node_try_ensure_runtime(node)) {
    return;
  }
  const NodeGeometryOpenMfx &storage = node_storage(*node);

  storage.runtime->setPluginPath(storage.plugin_path);
  storage.runtime->setEffectIndex(storage.effect_index);

  if (storage.runtime->mustUpdate()) {
    force_redeclare(ntree, node);
  }

  storage.runtime->clearMustUpdate();

  #if 0
  if (node->outputs.first == nullptr)
    return;

  bNodeSocket *out_socket_geometry = (bNodeSocket *)node->outputs.first;
  bNodeSocket *out_socket_base = out_socket_geometry->next;
  bNodeSocket *out_socket_olives = out_socket_base->next;

  // Stupid feature for the sake of the example: When there are too many
  // olives, we no longer output the fields!
  nodeSetSocketAvailability(ntree, out_socket_base, storage.olive_count < 25);
  nodeSetSocketAvailability(ntree, out_socket_olives, storage.olive_count < 25);
  #endif
}

#pragma region [Pizza]
static Mesh *create_pizza_mesh(const int olive_count,
                               const float radius,
                               const int base_div,
                               IndexRange &base_polys,
                               IndexRange &olives_polys)
{
  // (i) compute element counts
  int vert_count = base_div + olive_count * 4;
  int edge_count = base_div + olive_count * 4;
  int corner_count = base_div + olive_count * 4;
  int face_count = 1 + olive_count;

  // (ii) allocate memory
  Mesh *mesh = BKE_mesh_new_nomain(vert_count, edge_count, 0, corner_count, face_count);

  // (iii) fill in element buffers
  MutableSpan<MVert> verts{mesh->mvert, mesh->totvert};
  MutableSpan<MLoop> loops{mesh->mloop, mesh->totloop};
  MutableSpan<MEdge> edges{mesh->medge, mesh->totedge};
  MutableSpan<MPoly> polys{mesh->mpoly, mesh->totpoly};
  base_polys = IndexRange{0, 1};
  olives_polys = IndexRange{1, olive_count};

  // Base
  const float angle_delta = 2.0f * (M_PI / static_cast<float>(base_div));
  for (const int i : IndexRange(base_div)) {
    // Vertex coordinates
    const float angle = i * angle_delta;
    copy_v3_v3(verts[i].co, float3(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f));

    // Edge
    MEdge &edge = edges[i];
    edge.v1 = i;
    edge.v2 = (i + 1) % base_div;
    edge.flag = ME_EDGEDRAW | ME_EDGERENDER;

    // Corner
    MLoop &loop = loops[i];
    loop.e = i;
    loop.v = i;
  }
  // Face
  MPoly &poly = polys[0];
  poly.loopstart = 0;
  poly.totloop = base_div;

  // Olives
  const float angle_delta_olive = 2.0f * (M_PI / static_cast<float>(olive_count - 1));
  for (const int i : IndexRange(olive_count)) {
    const int offset = base_div + 4 * i;

    // Vertex coordinates
    float cx = 0, cy = 0;
    if (i > 0) { // (the olive #0 is at the center)
      const float angle = (i - 1) * angle_delta_olive;
      cx = std::cos(angle) * radius / 2;
      cy = std::sin(angle) * radius / 2;
    }
    copy_v3_v3(verts[offset + 0].co, float3(cx + 0.05f, cy + 0.05f, 0.01f));
    copy_v3_v3(verts[offset + 1].co, float3(cx - 0.05f, cy + 0.05f, 0.01f));
    copy_v3_v3(verts[offset + 2].co, float3(cx - 0.05f, cy - 0.05f, 0.01f));
    copy_v3_v3(verts[offset + 3].co, float3(cx + 0.05f, cy - 0.05f, 0.01f));

    for (const int k : IndexRange(4)) {
      // Edge
      MEdge &edge = edges[offset + k];
      edge.v1 = offset + k;
      edge.v2 = offset + (k + 1) % 4;
      edge.flag = ME_EDGEDRAW | ME_EDGERENDER;

      // Corner
      MLoop &loop = loops[offset + k];
      loop.e = offset + k;
      loop.v = offset + k;
    }

    // Face
    MPoly &poly = polys[1 + i];
    poly.loopstart = offset;
    poly.totloop = 4;
  }

  BLI_assert(BKE_mesh_is_valid(mesh));
  return mesh;
}

static void set_bool_face_field_output(GeoNodeExecParams &params, const char* attribute_name, const IndexRange &poly_range, Mesh *mesh)
{
  MeshComponent component;
  component.replace(mesh, GeometryOwnershipType::Editable);

  StrongAnonymousAttributeID id(attribute_name);
  blender::bke::SpanAttributeWriter<bool> attribute =
      component.attributes_for_write()->lookup_or_add_for_write_only_span<bool>(id.get(),
                                                                                 ATTR_DOMAIN_FACE);
  attribute.span.slice(poly_range).fill(true);
  attribute.finish();

  params.set_output(
      attribute_name,
      AnonymousAttributeFieldInput::Create<bool>(std::move(id), params.attribute_producer_name()));
}

static void set_float_field_output(GeoNodeExecParams &params,
                                       const char *attribute_name,
                                       const IndexRange &poly_range,
                                       Mesh *mesh,
                                       const eAttrDomain domain) // ATTR_DOMAIN_POINT
{
  MeshComponent component;
  component.replace(mesh, GeometryOwnershipType::Editable);

  StrongAnonymousAttributeID id(attribute_name);
  blender::bke::SpanAttributeWriter<float> attribute =
      component.attributes_for_write()->lookup_or_add_for_write_only_span<float>(id.get(), domain);
  attribute.span.slice(poly_range).fill(true);
  attribute.finish();

  params.set_output(
      attribute_name,
      AnonymousAttributeFieldInput::Create<float>(std::move(id), params.attribute_producer_name()));
}
#pragma endregion [Pizza]

static void node_geo_exec(GeoNodeExecParams params)
{
  TinyTimer::Timer timer;
  const NodeGeometryOpenMfx &storage = node_storage(params.node());
  OfxMeshEffectHandle effect = storage.runtime->effectInstance();

  if (effect == nullptr) {
    params.error_message_add(NodeWarningType::Info, TIP_("Could not load effect"));
    params.set_default_remaining_outputs();
    return;
  }

  auto &host = BlenderMfxHost::GetInstance();

  // 1. Check if identity
  bool isIdentity = true;
  char *inputToPassThrough = nullptr;
  host.IsIdentity(effect, &isIdentity, &inputToPassThrough);

  if (isIdentity) {
    const char* inputIdentifier = inputToPassThrough != nullptr ? inputToPassThrough : kOfxMeshMainInput;
    const OfxMeshInputStruct &input = effect->inputs[inputIdentifier];
    const OfxMeshInputStruct &output = effect->inputs[kOfxMeshMainOutput];
    GeometrySet geometry_set = params.extract_input<GeometrySet>(MFX_input_label(input));
    params.set_output(MFX_input_label(output), std::move(geometry_set));
    return;
  }

  // 2. Set inputs/outputs
  
  // Data that lives only for the call to the Cook action
  ResourceScope scope;
  std::vector<MeshInternalDataNode> inputInternalData(effect->inputs.count());
  std::vector<std::vector<GMutableSpan>> inputAttributeArrays(effect->inputs.count());

  MeshInternalDataNode *outputIt = nullptr;
  const char *outputLabel = nullptr;
  bool canCook = true;

  OfxMeshStruct AttributeIOMap;
  bool IOMapRequested = false;

  for (int i = 0; i < effect->inputs.count(); ++i) {
    OfxMeshInputStruct &input = effect->inputs[i];
    MeshInternalDataNode &inputData = inputInternalData[i];
    const char *label = MFX_input_label(input);
    inputData.header.is_input = input.name() != kOfxMeshMainOutput;
    inputData.header.type = BlenderMfxHost::CallbackContext::Node;

    if (inputData.header.is_input) {
      inputData.geo = params.extract_input<GeometrySet>(label);
      if (inputData.geo.has_mesh()) {
        // Evaluate requested attributes
        int attribCount = input.requested_attributes.count();
        inputData.requestedAttributes.resize(attribCount);

        // --------

        std::vector<GMutableSpan> &spans = inputAttributeArrays[i];
        //spans.resize(attribCount);
        GeometryComponent &component = inputData.geo.get_component_for_write(GEO_COMPONENT_TYPE_MESH);

        blender::bke::GeometryComponentFieldContext pointContext{component, ATTR_DOMAIN_POINT};
        blender::fn::FieldEvaluator pointEvaluator{
            pointContext, component.attribute_domain_size(ATTR_DOMAIN_POINT)};

        blender::bke::GeometryComponentFieldContext cornerContext{component, ATTR_DOMAIN_CORNER};
        blender::fn::FieldEvaluator cornerEvaluator{
            cornerContext, component.attribute_domain_size(ATTR_DOMAIN_CORNER)};

        blender::bke::GeometryComponentFieldContext faceContext{component, ATTR_DOMAIN_FACE};
        blender::fn::FieldEvaluator faceEvaluator{
            faceContext, component.attribute_domain_size(ATTR_DOMAIN_FACE)};

        // Evaluate requested attributes
        for (int j = 0; j < attribCount ; ++j) {
          const OfxAttributeStruct &def = input.requested_attributes[j];

          int64_t size = MFX_element_count(component, def.attachment());

          const CPPType &type = MFX_to_cpptype(def.type(), def.componentCount());
          void *evalBuffer = scope.linear_allocator().allocate(type.size() * size,
                                                                 type.alignment());
          spans.push_back(GMutableSpan(type, evalBuffer, size));

          GField field;
          // TODO switch on the number of components
          switch (def.type()) {
            case OfxAttributeStruct::AttributeType::Float:
              if (def.componentCount() == 1)
                field = params.get_input<Field<float>>(def.name());
              else if (def.componentCount() <= 3)
                field = params.get_input<Field<float3>>(def.name());
              else {
                BLI_assert(false);  // not implemented
              }
              
              break;
            case OfxAttributeStruct::AttributeType::Int:
              field = params.get_input<Field<int>>(def.name());
              break;
            default:
              BLI_assert(false); // not implemented
          }

          switch (def.attachment()) {
            case OfxAttributeStruct::AttributeAttachment::Point:
              
              pointEvaluator.add_with_destination(field, spans[j]);
              break;
            case OfxAttributeStruct::AttributeAttachment::Corner:
              cornerEvaluator.add_with_destination(field, spans[j]);
              break;
            case OfxAttributeStruct::AttributeAttachment::Face:
              faceEvaluator.add_with_destination(field, spans[j]);
              break;
            case OfxAttributeStruct::AttributeAttachment::Mesh:
              BLI_assert(false);  // not implemented
              break;
          }
        }

        pointEvaluator.evaluate();
        cornerEvaluator.evaluate();
        faceEvaluator.evaluate();

        for (int j = 0; j < attribCount; ++j) {
          const OfxAttributeStruct &def = input.requested_attributes[j];

          OfxAttributeStruct &attrib = inputData.requestedAttributes[j];
          attrib.deep_copy_from(def);
          attrib.properties[kOfxMeshAttribPropIsOwner].value[0].as_int = 0;

          void *data = spans[j].data();
          const CPPType &type = MFX_to_cpptype(def.type(), def.componentCount());
            
          attrib.properties[kOfxMeshAttribPropData].value[0].as_pointer = data;
          attrib.properties[kOfxMeshAttribPropStride].value[0].as_int = type.size();
        }

        // --------
      }
      else {
        canCook = false;
      }
    }
    else {
      outputLabel = label;
      outputIt = &inputInternalData[i];

      // Prepare expected attributes
      size_t requestedAttribCount = input.requested_attributes.count();

      inputData.requestedAttributes.resize(requestedAttribCount);
      inputData.outputAttributes.resize(requestedAttribCount);

      for (int j = 0; j < requestedAttribCount; ++j) {
        auto &attribData = inputData.requestedAttributes[j];
        attribData.deep_copy_from(input.requested_attributes[j]);
        inputData.outputAttributes[j] = StrongAnonymousAttributeID(attribData.name());
      }

      IOMapRequested = input.properties.find(kOfxInputPropRequestIOMap) != -1 &&
                       input.properties[kOfxInputPropRequestIOMap].value[0].as_int;

      if (IOMapRequested) {
        host.propertySuite->propSetPointer(&input.mesh.properties, kOfxMeshPropIOMap, 0, (void *)&AttributeIOMap);
      }
    }

    host.propertySuite->propSetPointer(&input.mesh.properties, kOfxMeshPropInternalData, 0, (void *)&inputData);
  }

  // 3. Set parameters
  for (int i = 0; i < effect->parameters.count(); ++i) {
    const OfxParamStruct &ofxParam = effect->parameters[i];
    MFX_node_extract_param(params, effect->parameters[i]);
  }

  // 4. Cook
  if (!canCook) {
    params.set_default_remaining_outputs();
    return;
  }

  TinyTimer::Timer subtimer;
  bool success = host.Cook(effect);
  PERF(1).add_sample(subtimer);

  if (!success) {
    MFX_node_set_message(params, effect);
    params.set_default_remaining_outputs();
    return;
  }

  if (nullptr != outputIt) {
    if (IOMapRequested) {
      Map<AttributeIDRef, AttributeKind> attributes_to_propagate;
      GeometrySet geometry_set = inputInternalData[0].geo;
      geometry_set.gather_attributes_for_propagation(
          {GEO_COMPONENT_TYPE_MESH}, GEO_COMPONENT_TYPE_MESH, false, attributes_to_propagate);
      // don't use map to propagate attributes that are calculated by blender on mesh create
      attributes_to_propagate.remove("position");
      attributes_to_propagate.remove("normal");
      attributes_to_propagate.remove("crease");

      MeshComponent &in_component = geometry_set.get_component_for_write<MeshComponent>();
      MeshComponent &out_component = outputIt->geo.get_component_for_write<MeshComponent>();

      AttributeProps originPointsPoolSize;
      const OfxAttributeStruct &ofxAttribOriginPointsPoolSize = AttributeIOMap.attributes[{
          OfxAttributeStruct::AttributeAttachment::Mesh, "OfxMeshAttribOriginPointsPoolSize"}];
      originPointsPoolSize.data = (char *)ofxAttribOriginPointsPoolSize
                                                    .properties[kOfxMeshAttribPropData]
                                                    .value[0]
                                                    .as_pointer;
      originPointsPoolSize.stride =
          ofxAttribOriginPointsPoolSize.properties[kOfxMeshAttribPropStride].value[0].as_int;

      AttributeProps originPointIndex;
      const OfxAttributeStruct &ofxAttribOriginPointIndex = AttributeIOMap.attributes[{
          OfxAttributeStruct::AttributeAttachment::Mesh, "OfxMeshAttribOriginPointIndex"}];
      originPointIndex.data =
          (char *)ofxAttribOriginPointIndex.properties[kOfxMeshAttribPropData].value[0].as_pointer;
      originPointIndex.stride =
          ofxAttribOriginPointIndex.properties[kOfxMeshAttribPropStride].value[0].as_int;

      AttributeProps originPointWeight;
      const OfxAttributeStruct &ofxAttribOriginPointWeight = AttributeIOMap.attributes[{
          OfxAttributeStruct::AttributeAttachment::Mesh, "OfxMeshAttribOriginPointWeight"}];
      originPointWeight.data = (char *)ofxAttribOriginPointWeight
                                                 .properties[kOfxMeshAttribPropData]
                                                 .value[0]
                                                 .as_pointer;
      originPointWeight.stride =
          ofxAttribOriginPointWeight.properties[kOfxMeshAttribPropStride].value[0].as_int;

      auto getOriginPointsPoolSize = [&](int outputPointIndex) {
        return *originPointsPoolSize.at<int>(outputPointIndex);
      };
      auto getOriginPointIndex = [&](int originPoint) {
        return *originPointIndex.at<int>(originPoint);
      };
      auto getOriginPointWeight = [&](int originPointIndex) {
        return *originPointWeight.at<float>(originPointIndex);
      };
      // TODO: extend this process to other types and other domains
      propagate_attributes(attributes_to_propagate,
                           in_component.attributes().value(),
                           out_component.attributes_for_write().value(),
                           {ATTR_DOMAIN_POINT},
                           {CD_PROP_FLOAT, CD_PROP_FLOAT2, CD_PROP_FLOAT3, CD_PROP_INT32, CD_PROP_BOOL},
                           getOriginPointsPoolSize,
                           getOriginPointIndex,
                           getOriginPointWeight);

      AttributeIOMap.free_owned_data();
      // meshHandle->properties.remove(findIOMap);
    }

    params.set_output(outputLabel, outputIt->geo);

    int requestedAttribCount = outputIt->requestedAttributes.size();
    for (int j = 0; j < requestedAttribCount; ++j) {
      const auto &attribInfo = outputIt->requestedAttributes[j];
      const auto &attrib = outputIt->outputAttributes[j];
      //if (params.output_is_required(attribInfo.name())) {
        // TODO: switch on type
        params.set_output(attribInfo.name(),
                          AnonymousAttributeFieldInput::Create<float>(
                              std::move(attrib), params.attribute_producer_name()));

        outputIt->outputAttributes[j] = {};
      //}
    }
  }

  PERF(0).add_sample(timer);
  std::cout << "Profiling:\n"
            << " - openmfx.node_geo_exec.total: " << PERF(0).summary() << "\n"
            << " - openmfx.node_geo_exec.cook: " << PERF(1).summary() << "\n"
            << std::flush;
}

static void node_gather_link_searches(GatherLinkSearchOpParams &UNUSED(params))
{
  // Deactivate the link search feature that is not compatible with dynamic
  // sockets yet (because GatherLinkSearchOpParams does not have access to the
  // node instance, only to the node type).
}

void node_read_data(BlendDataReader *reader, bNode *node)
{
  NodeGeometryOpenMfx &storage = node_storage(*node);
  storage.runtime = nullptr;
  MFX_node_try_ensure_runtime(node);
}

}  // namespace blender::nodes::node_geo_open_mfx_cc

// ----------------------------------------------------------------------------
// Registration (main entry point)

void register_node_type_geo_open_mfx()
{
  namespace file_ns = blender::nodes::node_geo_open_mfx_cc;

  static bNodeType ntype;
  geo_node_type_base(&ntype, GEO_NODE_OPEN_MFX, "OpenMfx Plugin", NODE_CLASS_GEOMETRY);
  ntype.declare = file_ns::node_declare;
  ntype.declaration_is_dynamic = true;
  node_type_init(&ntype, file_ns::node_init);
  node_type_update(&ntype, file_ns::node_update);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  node_type_storage(&ntype,
                    "NodeGeometryOpenMfx",
                    file_ns::node_free_storage,
                    file_ns::node_copy_storage);
  ntype.draw_buttons = file_ns::node_layout;
  ntype.gather_link_search_ops = file_ns::node_gather_link_searches;
  ntype.blend_read_data = file_ns::node_read_data;
  nodeRegisterType(&ntype);
}
