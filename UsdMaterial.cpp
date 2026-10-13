// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdMaterial.h"
#include "UsdAnari.h"
#include "UsdDevice.h"
#include "UsdSampler.h"
#include "UsdDataArray.h"
#include "UsdGeometry.h"

#include <algorithm>
#include <type_traits>

#define SamplerType ANARI_SAMPLER
using SamplerUsdType = AnariToUsdBridgedObject<SamplerType>::Type;

#define REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO(ParamName, ParamType0, ParamData) \
  REGISTER_PARAMETER_MULTITYPE_MACRO(ParamName, ParamType0, SamplerType, ANARI_STRING, ParamData)

DEFINE_PARAMETER_MAP(UsdMaterial, 
  REGISTER_PARAMETER_MACRO("name", ANARI_STRING, name)
  REGISTER_PARAMETER_MACRO("usd::name", ANARI_STRING, usdName)
  REGISTER_PARAMETER_MACRO("usd::time", ANARI_FLOAT64, timeStep)
  REGISTER_PARAMETER_MACRO("usd::timeVarying", ANARI_INT32, timeVarying)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.color", ANARI_FLOAT64, colorSamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.baseColor", ANARI_FLOAT64, colorSamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.opacity", ANARI_FLOAT64, opacitySamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.emissive", ANARI_FLOAT64, emissiveSamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.emissiveIntensity", ANARI_FLOAT64, emissiveIntensitySamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.roughness", ANARI_FLOAT64, roughnessSamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.metallic", ANARI_FLOAT64, metallicSamplerTimeStep)
  REGISTER_PARAMETER_MACRO("usd::time.sampler.ior", ANARI_FLOAT64, iorSamplerTimeStep)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("color", ANARI_FLOAT32_VEC3, color)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("baseColor", ANARI_FLOAT32_VEC3, color)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("opacity", ANARI_FLOAT32, opacity)
  REGISTER_PARAMETER_MACRO("alphaMode", ANARI_STRING, alphaMode)
  REGISTER_PARAMETER_MACRO("alphaCutoff", ANARI_FLOAT32, alphaCutoff)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("emissive", ANARI_FLOAT32_VEC3, emissiveColor)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("emissiveIntensity", ANARI_FLOAT32, emissiveIntensity)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("roughness", ANARI_FLOAT32, roughness)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("metallic", ANARI_FLOAT32, metallic)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("ior", ANARI_FLOAT32, ior)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("clearcoat", ANARI_FLOAT32, clearcoat)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("clearcoatRoughness", ANARI_FLOAT32, clearcoatRoughness)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("normal", ANARI_FLOAT32_VEC3, normal)
  REGISTER_PARAMETER_MATERIAL_MULTITYPE_MACRO("occlusion", ANARI_FLOAT32, occlusion)
)

constexpr UsdMaterial::ComponentPair UsdMaterial::componentParamNames[]; // Workaround for C++14's lack of inlining constexpr arrays

using DMI = UsdMaterial::MaterialDMI;

UsdMaterial::UsdMaterial(const char* name, const char* type, UsdDevice* device)
  : BridgedBaseObjectType(ANARI_MATERIAL, name, device)
{
  if (strEquals(type, "matte"))
  {
    isPbr = false;
  }
  else if (strEquals(type, "physicallyBased"))
  {
    isPbr = true;
  }
  else
  {
    device->reportStatus(this, ANARI_MATERIAL, ANARI_SEVERITY_ERROR, ANARI_STATUS_INVALID_ARGUMENT, "UsdMaterial '%s' intialization error: unknown material type", getName());
  }
}

UsdMaterial::~UsdMaterial()
{
#ifdef OBJECT_LIFETIME_EQUALS_USD_LIFETIME
  if(cachedBridge)
    cachedBridge->DeleteMaterial(usdHandle);
#endif
}

void UsdMaterial::remove(UsdDevice* device)
{
  applyRemoveFunc(device, &UsdBridge::DeleteMaterial);
}

namespace
{
  // Name of the primvar which the output of a transform sampler is written to
  template<typename ValueType>
  const char* transformSamplerOutput(const UsdSampler* sampler)
  {
    return std::is_same<ValueType, float>::value ? sampler->getScalarOutputName() : sampler->getName();
  }
}

template<typename ValueType>
void UsdMaterial::changeMaterialInputSource(const UsdMaterialMultiTypeParameter<ValueType>& param,
  MaterialDMI dataMemberId, const UsdGeometryData& geomParamData, bool forceUpdate,
  UsdDevice* device, const UsdLogInfo& logInfo)
{
  UsdSharedString* anariAttribStr = nullptr;
  param.Get(anariAttribStr); // If an input parameter is of string type, returns the pointer of the string (otherwise untouched)
  const char* anariAttrib = UsdSharedString::c_str(anariAttribStr);

  if(anariAttrib)
  {
    auto [hasMeshDependentAttribs, meshDependentAttribName] =
      GetGeomDependentAttributeName(anariAttrib, perInstance, device->getReadParams().useDisplayColorOpacity, geomParamData.attributeNames, MAX_ATTRIBS, logInfo);

    bool hasFixedAttributeType = HasFixedAttributeType(anariAttrib);

    if(forceUpdate || hasMeshDependentAttribs || !hasFixedAttributeType)
      materialInputAttributes.push_back(UsdBridge::MaterialInputAttribName(dataMemberId, meshDependentAttribName));
  }
  else if(param.type == SamplerType)
  {
    UsdSampler* sampler = nullptr;
    if (param.Get(sampler))
    {
      if(sampler->isTransform()) // Read from the primvar which has been written for the geometry
        materialInputAttributes.push_back(UsdBridge::MaterialInputAttribName(dataMemberId, transformSamplerOutput<ValueType>(sampler)));
      else
        sampler->updateBoundParameters(perInstance, geomParamData.attributeNames, MAX_ATTRIBS, device);
    }
  }
}

template<typename ValueType>
bool UsdMaterial::getSamplerRefData(const UsdMaterialMultiTypeParameter<ValueType>& param, double refTimeStep, MaterialDMI dataMemberId, UsdDevice* device, const UsdLogInfo& logInfo)
{
  UsdSampler* sampler = nullptr;
  param.Get(sampler);

  if(sampler && !sampler->isTransform())
  {
    const UsdSamplerData& samplerParamData = sampler->getReadParams();

    double worldTimeStep = device->getReadParams().timeStep;
    double samplerObjTimeStep = samplerParamData.timeStep;
    double samplerRefTime = selectRefTime(refTimeStep, samplerObjTimeStep, worldTimeStep);
    
    // Reading child (sampler) data in the material has the consequence that the sampler's parameters as they were last committed are in effect "part of" the material parameter set, at this point of commit. 
    // So in case a sampler at a particular timestep is referenced from a material at two different world timesteps 
    // - ie. for this world timestep, a particular timestep of an image already committed and subsequently referenced at some other previous world timestep is reused - 
    // the user needs to make sure that not only the timestep is set correctly on the sampler for the commit (which is by itself lightweight, as it does not trigger a full commit), 
    // but also that the parameters read here have been re-set on the sampler to the values belonging to the referenced timestep, as if there is no USD representation of a sampler object. 
    // Setting those parameters will in turn trigger a full commit of the sampler object, which is in theory inefficient.
    // However, in case of a sampler this is not a problem in practice; data transfer is only a concern when the filename is *not* set, at which point a relative file corresponding
    // to the sampler timestep will be automatically chosen and set for the material, without the sampler object requiring any updates. 
    // In case a filename *is* set, only the filename is used and no data transfer/file io operations are performed.
    //const char* imageUrl = UsdSharedString::c_str(samplerParamData.imageUrl); // not required anymore since all materials are a graph

    int imageNumComponents = 4;
    if(samplerParamData.imageData)
    {
      imageNumComponents = static_cast<int>(anari::componentsOf(samplerParamData.imageData->getType()));
    }
    UsdSamplerRefData samplerRefData = {imageNumComponents, samplerRefTime, dataMemberId,
      dataMemberId == DMI::OPACITY && sampler->writesOpacityToAlpha()};

    samplerHandles.push_back(sampler->getUsdHandle());
    samplerRefDatas.push_back(samplerRefData);
  }

  return false;
}

template<typename ValueType>
void UsdMaterial::assignParameterToMaterialInput(const UsdMaterialMultiTypeParameter<ValueType>& param, 
  UsdBridgeMaterialData::MaterialInput<ValueType>& matInput, bool useDisplayColorOpacity, const UsdLogInfo& logInfo)
{
  param.Get(matInput.Value);

  UsdSharedString* anariAttribStr = nullptr;
  matInput.SrcAttrib = param.Get(anariAttribStr) ? 
    AnariAttributeToUsdName(anariAttribStr->c_str(), perInstance, useDisplayColorOpacity, logInfo) :
    nullptr; 

  UsdSampler* sampler = nullptr;
  matInput.Sampler = param.Get(sampler);

  // The output of a transform sampler is a primvar of the geometry, see bakeTransformSamplers()
  if(sampler && sampler->isTransform())
  {
    matInput.SrcAttrib = transformSamplerOutput<ValueType>(sampler);
    matInput.Sampler = false;
  }
}

namespace
{
  template<typename ParamType>
  UsdSampler* transformSamplerOf(const ParamType& param)
  {
    UsdSampler* sampler = nullptr;
    return (param.Get(sampler) && sampler && sampler->isTransform()) ? sampler : nullptr;
  }
}

void UsdMaterial::bakeTransformSamplers(UsdGeometry* geometry, UsdDevice* device)
{
  const UsdMaterialData& paramData = getReadParams();

  UsdSampler* colorSamplers[] = {
    transformSamplerOf(paramData.color),
    transformSamplerOf(paramData.emissiveColor)
  };
  UsdSampler* scalarSamplers[] = {
    transformSamplerOf(paramData.opacity),
    transformSamplerOf(paramData.emissiveIntensity),
    transformSamplerOf(paramData.roughness),
    transformSamplerOf(paramData.metallic),
    transformSamplerOf(paramData.ior),
    transformSamplerOf(paramData.clearcoat),
    transformSamplerOf(paramData.clearcoatRoughness)
  };

  for(UsdSampler* sampler : colorSamplers)
  {
    if(sampler)
      sampler->bakeAttribute(geometry, false, device);
  }
  for(UsdSampler* sampler : scalarSamplers)
  {
    if(sampler)
      sampler->bakeAttribute(geometry, true, device);
  }

  // Texture coordinates which no node of the shading network can compute
  auto bakeCoordinatesOf = [geometry, device](const auto& param)
  {
    UsdSampler* sampler = nullptr;
    if(param.Get(sampler) && sampler)
      sampler->bakeCoordinates(geometry, device);
  };

  bakeCoordinatesOf(paramData.color);
  bakeCoordinatesOf(paramData.opacity);
  bakeCoordinatesOf(paramData.emissiveColor);
  bakeCoordinatesOf(paramData.emissiveIntensity);
  bakeCoordinatesOf(paramData.roughness);
  bakeCoordinatesOf(paramData.metallic);
  bakeCoordinatesOf(paramData.ior);
  bakeCoordinatesOf(paramData.clearcoat);
  bakeCoordinatesOf(paramData.clearcoatRoughness);
  bakeCoordinatesOf(paramData.normal);
  bakeCoordinatesOf(paramData.occlusion);
}

void UsdMaterial::declareOpacitySampler(UsdDevice* device)
{
  const UsdMaterialData& paramData = getReadParams();

  UsdSampler* opacitySampler = nullptr;
  UsdSampler* colorSampler = nullptr;
  paramData.opacity.Get(opacitySampler);
  paramData.color.Get(colorSampler);

  // The alpha of the texture of the color is its own; the MDL material connects otherwise
  if(opacitySampler && !opacitySampler->isTransform())
    opacitySampler->setOpacityInAlpha(opacitySampler != colorSampler && !device->getReadParams().outputMdlShader);
}

uint32_t UsdMaterial::getTexCoordAttributes() const
{
  const UsdMaterialData& paramData = getReadParams();

  uint32_t attributes = 0;
  auto addSamplerOf = [&attributes](const auto& param)
  {
    UsdSampler* sampler = nullptr;
    if(param.Get(sampler) && sampler)
    {
      const int attribIndex = sampler->getTexCoordAttribute();
      if(attribIndex >= 0)
        attributes |= 1u << attribIndex;
    }
  };

  addSamplerOf(paramData.color);
  addSamplerOf(paramData.opacity);
  addSamplerOf(paramData.emissiveColor);
  addSamplerOf(paramData.emissiveIntensity);
  addSamplerOf(paramData.roughness);
  addSamplerOf(paramData.metallic);
  addSamplerOf(paramData.ior);
  addSamplerOf(paramData.clearcoat);
  addSamplerOf(paramData.clearcoatRoughness);
  addSamplerOf(paramData.normal);
  addSamplerOf(paramData.occlusion);

  return attributes;
}

void UsdMaterial::updateBoundParameters(bool boundToInstance, const UsdGeometryData& geomParamData, UsdDevice* device)
{ 
  // The geometry to which a material binds has an effect on:
  // - attribute reader (geom primvar) names,
  // - attribute reader output types
  // These will have been setup initially in commit() with UsdBridge::SetMaterialData using UpdateUsdMaterial, which is correct for:
  // - the attribute reader names except those that are geometry type dependent
  // - the types of those attributes that are predetermined (points, normals)
  // For anything else or in case no geometry has been bound before,
  // the material's attribute reader needs to be updated to use the correct type given the new geometry.
  // This is done here with UsdMaterial::changeMaterialInputSource().

  perInstance = boundToInstance;

  UsdBridge* usdBridge = device->getUsdBridge();
  const UsdMaterialData& paramData = getReadParams();
  
  UsdLogInfo logInfo = {device, this, ANARI_MATERIAL, getName()};

  double worldTimeStep = device->getReadParams().timeStep;
  double dataTimeStep = selectObjTime(paramData.timeStep, worldTimeStep);

  // Fix up any parameters that have a geometry-type-dependent name set as source attribute
  materialInputAttributes.clear();

  bool forceUpdate = !isBound;
  changeMaterialInputSource(paramData.color, DMI::DIFFUSE, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.opacity, DMI::OPACITY, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.emissiveColor, DMI::EMISSIVECOLOR, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.emissiveIntensity, DMI::EMISSIVEINTENSITY, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.roughness, DMI::ROUGHNESS, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.metallic, DMI::METALLIC, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.ior, DMI::IOR, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.clearcoat, DMI::CLEARCOAT, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.clearcoatRoughness, DMI::CLEARCOATROUGHNESS, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.normal, DMI::NORMAL, geomParamData, forceUpdate, device, logInfo);
  changeMaterialInputSource(paramData.occlusion, DMI::OCCLUSION, geomParamData, forceUpdate, device, logInfo);

  DMI timeVarying;
  setMaterialTimeVarying(timeVarying);

  // Fixup attribute name and type depending on the newly bound geometry
  if(materialInputAttributes.size())
    usdBridge->ChangeMaterialInputAttributes(usdHandle, materialInputAttributes.data(), materialInputAttributes.size(), dataTimeStep, timeVarying);

  isBound = true;
}

bool UsdMaterial::deferCommit(UsdDevice* device)
{
  //const UsdMaterialData& paramData = getReadParams();

  //if(UsdObjectNotInitialized<SamplerUsdType>(paramData.color.type == SamplerType))
  //{
  //  return true;
  //}
  return false;
}

bool UsdMaterial::doCommitData(UsdDevice* device)
{
  UsdBridge* usdBridge = device->getUsdBridge();

  if(!device->getReadParams().outputMaterial)
    return false;

  bool isNew = false;
  if (!usdHandle.value)
    isNew = usdBridge->CreateMaterial(getName(), usdHandle);

  if (paramChanged || isNew)
  {
    const UsdMaterialData& paramData = getReadParams();

    double worldTimeStep = device->getReadParams().timeStep;
    double dataTimeStep = selectObjTime(paramData.timeStep, worldTimeStep);

    UsdBridgeMaterialData matData;
    matData.IsPbr = isPbr;
    matData.AlphaMode = AnariToUsdAlphaMode(UsdSharedString::c_str(paramData.alphaMode));
    matData.AlphaCutoff = paramData.alphaCutoff;

    bool useDisplayColorOpacity = device->getReadParams().useDisplayColorOpacity;

    UsdLogInfo logInfo = {device, this, ANARI_MATERIAL, getName()};
    
    assignParameterToMaterialInput(paramData.color, matData.Diffuse, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.opacity, matData.Opacity, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.emissiveColor, matData.Emissive, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.emissiveIntensity, matData.EmissiveIntensity, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.roughness, matData.Roughness, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.metallic, matData.Metallic, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.ior, matData.Ior, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.clearcoat, matData.Clearcoat, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.clearcoatRoughness, matData.ClearcoatRoughness, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.normal, matData.Normal, useDisplayColorOpacity, logInfo);
    assignParameterToMaterialInput(paramData.occlusion, matData.Occlusion, useDisplayColorOpacity, logInfo);

    // A blended ANARI material covers the fraction `opacity` of its surface and lets the rest
    // pass unchanged. The opacity of a preview surface is that of the body below its specular
    // response, which a metal does not have: a blended metal reads as a full mirror there. The
    // same surface as a preview surface: the metal covers its fraction as `metallic`, the rest
    // is a dielectric whose opacity is what is left of the diffuse part. Where no dielectric is
    // left, nothing has an index of refraction: what passes, passes straight.
    // The opacity of the MDL material is a coverage as well, so this is for stages without it.
    const bool constantCoverage = !matData.Opacity.SrcAttrib && !matData.Opacity.Sampler
      && !matData.Metallic.SrcAttrib && !matData.Metallic.Sampler;
    if(matData.AlphaMode == UsdBridgeMaterialData::AlphaModes::BLEND && constantCoverage
      && !device->getReadParams().outputMdlShader)
    {
      const float coverage = std::min(std::max(matData.Opacity.Value, 0.0f), 1.0f);
      const float metal = std::min(std::max(matData.Metallic.Value, 0.0f), 1.0f);
      if(coverage < 1.0f && (metal > 0.0f || coverage == 0.0f))
      {
        const float metalCoverage = coverage * metal;
        const float dielectricCoverage = coverage * (1.0f - metal);
        matData.Metallic.Value = metalCoverage;
        matData.Opacity.Value = (metalCoverage < 1.0f) ? dielectricCoverage / (1.0f - metalCoverage) : 1.0f;
        if(dielectricCoverage <= 0.0f && !matData.Ior.SrcAttrib && !matData.Ior.Sampler)
          matData.Ior.Value = 1.0f;
      }
    }

    setMaterialTimeVarying(matData.TimeVarying);

    usdBridge->SetMaterialData(usdHandle, matData, dataTimeStep);

    paramChanged = false;

    // Only commit refs when the material actually contains a texture
    return paramData.color.type == SamplerType || paramData.opacity.type == SamplerType
      || paramData.emissiveColor.type == SamplerType || paramData.roughness.type == SamplerType
      || paramData.metallic.type == SamplerType || paramData.clearcoat.type == SamplerType
      || paramData.clearcoatRoughness.type == SamplerType || paramData.normal.type == SamplerType
      || paramData.occlusion.type == SamplerType;
  }

  return false;
}

void UsdMaterial::doCommitRefs(UsdDevice* device)
{
  UsdBridge* usdBridge = device->getUsdBridge();
  const UsdMaterialData& paramData = getReadParams();

  double worldTimeStep = device->getReadParams().timeStep;

  samplerHandles.clear();
  samplerRefDatas.clear();

  UsdLogInfo logInfo = {device, this, ANARI_MATERIAL, getName()};

  getSamplerRefData(paramData.color, paramData.colorSamplerTimeStep, DMI::DIFFUSE, device, logInfo);
  getSamplerRefData(paramData.opacity, paramData.opacitySamplerTimeStep, DMI::OPACITY, device, logInfo);
  getSamplerRefData(paramData.emissiveColor, paramData.emissiveSamplerTimeStep, DMI::EMISSIVECOLOR, device, logInfo);
  getSamplerRefData(paramData.emissiveIntensity, paramData.emissiveIntensitySamplerTimeStep, DMI::EMISSIVEINTENSITY, device, logInfo);
  getSamplerRefData(paramData.roughness, paramData.roughnessSamplerTimeStep, DMI::ROUGHNESS, device, logInfo);
  getSamplerRefData(paramData.metallic, paramData.metallicSamplerTimeStep, DMI::METALLIC, device, logInfo);
  getSamplerRefData(paramData.ior, paramData.iorSamplerTimeStep, DMI::IOR, device, logInfo);

  const double noRefTime = std::numeric_limits<double>::quiet_NaN();
  getSamplerRefData(paramData.clearcoat, noRefTime, DMI::CLEARCOAT, device, logInfo);
  getSamplerRefData(paramData.clearcoatRoughness, noRefTime, DMI::CLEARCOATROUGHNESS, device, logInfo);
  getSamplerRefData(paramData.normal, noRefTime, DMI::NORMAL, device, logInfo);
  getSamplerRefData(paramData.occlusion, noRefTime, DMI::OCCLUSION, device, logInfo);

  if(samplerHandles.size())
    usdBridge->SetSamplerRefs(usdHandle, samplerHandles.data(), samplerHandles.size(), worldTimeStep, samplerRefDatas.data());
  else
    usdBridge->DeleteSamplerRefs(usdHandle, worldTimeStep);
}

void UsdMaterial::setMaterialTimeVarying(UsdBridgeMaterialData::DataMemberId& timeVarying)
{
  timeVarying = DMI::ALL
    & (isTimeVarying(CType::COLOR) ? DMI::ALL : ~DMI::DIFFUSE)
    & (isTimeVarying(CType::OPACITY) ? DMI::ALL : ~DMI::OPACITY)
    & (isTimeVarying(CType::EMISSIVE) ? DMI::ALL : ~DMI::EMISSIVECOLOR)
    & (isTimeVarying(CType::EMISSIVEFACTOR) ? DMI::ALL : ~DMI::EMISSIVEINTENSITY)
    & (isTimeVarying(CType::ROUGHNESS) ? DMI::ALL : ~DMI::ROUGHNESS)
    & (isTimeVarying(CType::METALLIC) ? DMI::ALL : ~DMI::METALLIC)
    & (isTimeVarying(CType::IOR) ? DMI::ALL : ~DMI::IOR)
    // No component stands for these inputs: they are part of the material as it is defined
    & ~(DMI::CLEARCOAT | DMI::CLEARCOATROUGHNESS | DMI::NORMAL | DMI::OCCLUSION);
}