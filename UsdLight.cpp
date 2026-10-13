// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdLight.h"
#include "UsdAnari.h"
#include "UsdDevice.h"

#include <algorithm>
#include <cmath>

DEFINE_PARAMETER_MAP(UsdLight,
  REGISTER_PARAMETER_MACRO("name", ANARI_STRING, name)
  REGISTER_PARAMETER_MACRO("usd::name", ANARI_STRING, usdName)
  REGISTER_PARAMETER_MACRO("usd::timeVarying", ANARI_INT32, timeVarying)
  REGISTER_PARAMETER_MACRO("color", ANARI_FLOAT32_VEC3, color)
  REGISTER_PARAMETER_MACRO("position", ANARI_FLOAT32_VEC3, position)
  REGISTER_PARAMETER_MACRO("direction", ANARI_FLOAT32_VEC3, direction)
  REGISTER_PARAMETER_MACRO("irradiance", ANARI_FLOAT32, irradiance)
  REGISTER_PARAMETER_MACRO("intensity", ANARI_FLOAT32, intensity)
  REGISTER_PARAMETER_MACRO("power", ANARI_FLOAT32, power)
  REGISTER_PARAMETER_MACRO("radiance", ANARI_FLOAT32, radiance)
  REGISTER_PARAMETER_MACRO("angularDiameter", ANARI_FLOAT32, angularDiameter)
  REGISTER_PARAMETER_MACRO("radius", ANARI_FLOAT32, radius)
  REGISTER_PARAMETER_MACRO("openingAngle", ANARI_FLOAT32, openingAngle)
  REGISTER_PARAMETER_MACRO("falloffAngle", ANARI_FLOAT32, falloffAngle)
  REGISTER_PARAMETER_MACRO("edge1", ANARI_FLOAT32_VEC3, edge1)
  REGISTER_PARAMETER_MACRO("edge2", ANARI_FLOAT32_VEC3, edge2)
)

namespace
{
  UsdBridgeLightType GetLightType(const char* type)
  {
    UsdBridgeLightType lightType;

    if (strEquals(type, "directional"))
      lightType = UsdBridgeLightType::DIRECTIONAL;
    else if (strEquals(type, "point"))
      lightType = UsdBridgeLightType::POINT;
    else if (strEquals(type, "spot"))
      lightType = UsdBridgeLightType::SPOT;
    else if (strEquals(type, "quad"))
      lightType = UsdBridgeLightType::QUAD;
    else
      lightType = UsdBridgeLightType::DOME;

    return lightType;
  }

  // UsdLux and ANARI both describe light radiometrically, so quantities pass through without
  // a photometric factor. With inputs:normalize=true on the UsdLuxLight, the authored
  // intensity is the radiance of a light of unit size, which makes it for a
  //   - DistantLight: its irradiance (W/m^2)
  //   - SphereLight:  4 times its radiant intensity (W/sr): the radiance L of a sphere of
  //                   radius r is intensity / (4 pi r^2), and it shows a disk of pi r^2
  //   - RectLight:    its radiance times its area, the intensity along its normal (W/sr)
  //   - DomeLight:    its radiance (W/sr/m^2)
  // In each case but the last, pi times the intensity is the emitted power per unit of
  // the measure which the light is normalized by.

  // Decompose an RGB color into chromaticity (in [0,1]) and a positive
  // magnitude (the max channel). The magnitude is folded into the light
  // intensity so inputs:color stays a pure chromaticity, which is what
  // UsdLux conventionally expects.
  inline void splitColor(const UsdFloat3& in, UsdFloat3& chroma, float& mag)
  {
    const float m = std::max({in.Data[0], in.Data[1], in.Data[2]});
    if (m > 0.0f)
    {
      chroma.Data[0] = in.Data[0] / m;
      chroma.Data[1] = in.Data[1] / m;
      chroma.Data[2] = in.Data[2] / m;
      mag = m;
    }
    else
    {
      chroma = UsdFloat3{1.0f, 1.0f, 1.0f};
      mag = 0.0f;
    }
  }

  // Split a (large) magnitude into (intensity, exposure) such that
  //   intensity * 2^exposure == magnitude (within float precision)
  // and intensity stays at or below 2^16 for human-readable authoring in
  // DCC tools. Exposure stays at 0 for any reasonable everyday magnitude.
  inline void splitIntensityExposure(float magnitude, float& intensity, float& exposure)
  {
    constexpr float READABLE_MAX = 65536.0f; // 2^16
    if (magnitude > READABLE_MAX && std::isfinite(magnitude))
    {
      exposure = std::ceil(std::log2(magnitude)) - 16.0f;
      intensity = magnitude * std::exp2(-exposure);
    }
    else
    {
      exposure = 0.0f;
      intensity = magnitude;
    }
  }
}

UsdLight::UsdLight(const char* name, const char* type, UsdDevice* device)
  : BridgedBaseObjectType(ANARI_LIGHT, name, device)
{
  lightType = GetLightType(type);

}

UsdLight::~UsdLight()
{}

void UsdLight::remove(UsdDevice* device)
{
  applyRemoveFunc(device, &UsdBridge::DeleteLight);
}

void UsdLight::CopyParameters(const UsdLightData& paramData, UsdBridgeDirectionalLightData& lightData)
{
  typedef UsdBridgeDirectionalLightData::DataMemberId DMI;

  // ANARI 1.x KHR_LIGHT_DIRECTIONAL precedence: irradiance (W/m^2) wins over
  // radiance (W/sr/m^2). When only radiance is given, multiply by the source's
  // solid angle to recover an equivalent irradiance. With angularDiameter == 0
  // the directional light emits nothing per spec; we fall back to the ANARI
  // default irradiance of 1 W/m^2 to keep the scene visible.
  float radiometric = 1.0f;
  if (paramData.irradiance != -1.0f)
  {
    radiometric = paramData.irradiance;
  }
  else if (paramData.radiance != -1.0f)
  {
    const float halfAngle = 0.5f * paramData.angularDiameter;
    const float solidAngle = 2.0f * float(M_PI) * (1.0f - std::cos(halfAngle));
    if (solidAngle > 0.0f)
      radiometric = paramData.radiance * solidAngle;
  }

  UsdFloat3 chroma; float colorMag = 1.0f;
  splitColor(paramData.color, chroma, colorMag);

  const float magnitude = radiometric * colorMag;
  splitIntensityExposure(magnitude, lightData.Intensity, lightData.Exposure);

  lightData.Color = chroma;
  lightData.Direction = paramData.direction;
  lightData.AngularDiameter = paramData.angularDiameter;

  lightData.TimeVarying = DMI::ALL
    & (isTimeVarying(CType::COLOR) ? DMI::ALL : ~DMI::COLOR)
    & ((isTimeVarying(CType::INTENSITY) || isTimeVarying(CType::IRRADIANCE) || isTimeVarying(CType::RADIANCE))
        ? DMI::ALL : ~DMI::INTENSITY)
    & (isTimeVarying(CType::DIRECTION) ? DMI::ALL : ~DMI::DIRECTION)
    & (isTimeVarying(CType::ANGULAR_DIAMETER) ? DMI::ALL : ~DMI::ANGULAR_DIAMETER);
}

void UsdLight::CopyParameters(const UsdLightData& paramData,UsdBridgePointLightData& lightData)
{
  typedef UsdBridgePointLightData::DataMemberId DMI;

  // ANARI 1.x KHR_LIGHT_POINT precedence: intensity (W/sr) > power (W) >
  // radiance (W/sr/m^2). For an isotropic point source, power = intensity * 4*pi.
  // The radiance fallback is a coarse approximation (assumes unit emitter area),
  // but only kicks in when the user has not set the more direct parameters.
  float radiometric = 1.0f;
  if (paramData.intensity != -1.0f)
    radiometric = paramData.intensity;
  else if (paramData.power != -1.0f)
    radiometric = paramData.power / (4.0f * float(M_PI));
  else if (paramData.radiance != -1.0f)
    radiometric = paramData.radiance;

  UsdFloat3 chroma; float colorMag = 1.0f;
  splitColor(paramData.color, chroma, colorMag);

  if(lightType == UsdBridgeLightType::SPOT)
  {
    // ANARI spreads the power of a spot light over the solid angle of its cone
    const float halfAngle = std::min(std::max(0.5f * paramData.openingAngle, 0.0f), float(M_PI));
    if(paramData.intensity == -1.0f && paramData.power != -1.0f)
    {
      const float solidAngle = 2.0f * float(M_PI) * (1.0f - std::cos(halfAngle));
      radiometric = solidAngle > 0.0f ? paramData.power / solidAngle : 0.0f;
    }

    lightData.Direction = paramData.direction;
    lightData.ConeAngle = halfAngle * (180.0f / float(M_PI));
    // The softness is the part of the cone over which the light falls off, measured in
    // the cosine of the angle to the axis, as renderers evaluate it
    const float cosOuter = std::cos(halfAngle);
    const float cosInner = std::cos(std::max(halfAngle - std::max(paramData.falloffAngle, 0.0f), 0.0f));
    lightData.ConeSoftness = cosOuter < 1.0f
      ? std::min(std::max((cosInner - cosOuter) / (1.0f - cosOuter), 0.0f), 1.0f)
      : 0.0f;
  }

  // Four times the intensity, see the note on units above
  const float magnitude = 4.0f * radiometric * colorMag;
  splitIntensityExposure(magnitude, lightData.Intensity, lightData.Exposure);

  lightData.Color = chroma;
  lightData.Position = paramData.position;
  lightData.Radius = paramData.radius;

  lightData.TimeVarying = DMI::ALL
    & (isTimeVarying(CType::COLOR) ? DMI::ALL : ~DMI::COLOR)
    & ((isTimeVarying(CType::INTENSITY) || isTimeVarying(CType::POWER) || isTimeVarying(CType::RADIANCE))
        ? DMI::ALL : ~DMI::INTENSITY)
    & (isTimeVarying(CType::POSITION) ? DMI::ALL : ~DMI::POSITION)
    & (isTimeVarying(CType::RADIUS) ? DMI::ALL : ~DMI::RADIUS);
}

void UsdLight::CopyParameters(const UsdLightData& paramData, UsdBridgeRectLightData& lightData)
{
  typedef UsdBridgeRectLightData::DataMemberId DMI;

  const UsdFloat3& e1 = paramData.edge1;
  const UsdFloat3& e2 = paramData.edge2;
  const float cross[3] = {
    e1.Data[1]*e2.Data[2] - e1.Data[2]*e2.Data[1],
    e1.Data[2]*e2.Data[0] - e1.Data[0]*e2.Data[2],
    e1.Data[0]*e2.Data[1] - e1.Data[1]*e2.Data[0]};
  const float area = std::sqrt(cross[0]*cross[0] + cross[1]*cross[1] + cross[2]*cross[2]);

  // ANARI 1.x KHR_LIGHT_QUAD: 'radiance' (W/sr/m^2), 'intensity' along the normal (W/sr),
  // or the 'power' (W) of the one-sided diffuse emitter. The normalized rect light takes
  // the intensity along the normal, see the note on units above.
  float radiometric = area;
  if (paramData.radiance != -1.0f)
    radiometric = paramData.radiance * area;
  else if (paramData.intensity != -1.0f)
    radiometric = paramData.intensity;
  else if (paramData.power != -1.0f)
    radiometric = paramData.power / float(M_PI);

  UsdFloat3 chroma; float colorMag = 1.0f;
  splitColor(paramData.color, chroma, colorMag);

  const float magnitude = radiometric * colorMag;
  splitIntensityExposure(magnitude, lightData.Intensity, lightData.Exposure);

  lightData.Color = chroma;
  lightData.Position = paramData.position;
  lightData.Edge1 = paramData.edge1;
  lightData.Edge2 = paramData.edge2;

  lightData.TimeVarying = DMI::ALL
    & (isTimeVarying(CType::COLOR) ? DMI::ALL : ~DMI::COLOR)
    & ((isTimeVarying(CType::INTENSITY) || isTimeVarying(CType::POWER) || isTimeVarying(CType::RADIANCE))
        ? DMI::ALL : ~DMI::INTENSITY)
    & (isTimeVarying(CType::POSITION) ? DMI::ALL : ~DMI::POSITION);
}

void UsdLight::CopyParameters(const UsdLightData& paramData, UsdBridgeDomeLightData& lightData)
{
  typedef UsdBridgeDomeLightData::DataMemberId DMI;

  // ANARI 1.x KHR_LIGHT_HDRI uses 'radiance' (W/sr/m^2). 'intensity' is also
  // accepted as a legacy alias when explicitly set.
  float radiometric = 1.0f;
  if (paramData.intensity != -1.0f)
    radiometric = paramData.intensity;
  else if (paramData.radiance != -1.0f)
    radiometric = paramData.radiance;

  UsdFloat3 chroma; float colorMag = 1.0f;
  splitColor(paramData.color, chroma, colorMag);

  const float magnitude = radiometric * colorMag;
  splitIntensityExposure(magnitude, lightData.Intensity, lightData.Exposure);

  lightData.Color = chroma;

  lightData.TimeVarying = DMI::ALL
    & (isTimeVarying(CType::COLOR) ? DMI::ALL : ~DMI::COLOR)
    & ((isTimeVarying(CType::INTENSITY) || isTimeVarying(CType::RADIANCE))
        ? DMI::ALL : ~DMI::INTENSITY);
}

template<typename LightDataType>
void UsdLight::SetLightData(UsdBridge* usdBridge, UsdLightHandle usdHandle, const UsdLightData& paramData, double timeStep)
{
  LightDataType lightData;
  CopyParameters(paramData, lightData);

  usdBridge->SetLightData(usdHandle, lightData, timeStep);
}

bool UsdLight::deferCommit(UsdDevice* device) 
{ 
  return false; 
}

bool UsdLight::doCommitData(UsdDevice* device) 
{
  UsdBridge* usdBridge = device->getUsdBridge();
  const UsdLightData& paramData = getReadParams();
  double timeStep = device->getReadParams().timeStep;

  bool isNew = false;
  if (!usdHandle.value)
    isNew = usdBridge->CreateLight(getName(), lightType, usdHandle);

  if (paramChanged || isNew)
  {
    switch(lightType)
    {
      case UsdBridgeLightType::DIRECTIONAL: SetLightData<UsdBridgeDirectionalLightData>(usdBridge, usdHandle, paramData, timeStep); break;
      case UsdBridgeLightType::POINT:
      case UsdBridgeLightType::SPOT: SetLightData<UsdBridgePointLightData>(usdBridge, usdHandle, paramData, timeStep); break;
      case UsdBridgeLightType::QUAD: SetLightData<UsdBridgeRectLightData>(usdBridge, usdHandle, paramData, timeStep); break;
      case UsdBridgeLightType::DOME: SetLightData<UsdBridgeDomeLightData>(usdBridge, usdHandle, paramData, timeStep); break;
      default: break;
    }

    paramChanged = false;
  }

  return false;
}