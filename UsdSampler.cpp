// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#include "UsdSampler.h"
#include "UsdAnari.h"
#include "UsdDevice.h"
#include "UsdDataArray.h"
#include "UsdGeometry.h"

#include "helium/helium_math.h"

#include <algorithm>
#include <cmath>

DEFINE_PARAMETER_MAP(UsdSampler, 
  REGISTER_PARAMETER_MACRO("name", ANARI_STRING, name)
  REGISTER_PARAMETER_MACRO("usd::name", ANARI_STRING, usdName)
  REGISTER_PARAMETER_MACRO("usd::time", ANARI_FLOAT64, timeStep)
  REGISTER_PARAMETER_MACRO("usd::timeVarying", ANARI_INT32, timeVarying)
  REGISTER_PARAMETER_MACRO("usd::imageUrl", ANARI_STRING, imageUrl)
  REGISTER_PARAMETER_MACRO("inAttribute", ANARI_STRING, inAttribute)
  REGISTER_PARAMETER_MACRO("image", ANARI_ARRAY, imageData)
  REGISTER_PARAMETER_MACRO("wrapMode", ANARI_STRING, wrapS)
  REGISTER_PARAMETER_MACRO("wrapMode1", ANARI_STRING, wrapS)
  REGISTER_PARAMETER_MACRO("wrapMode2", ANARI_STRING, wrapT)
  REGISTER_PARAMETER_MACRO("wrapMode3", ANARI_STRING, wrapR)
  REGISTER_PARAMETER_MACRO("inTransform", ANARI_FLOAT32_MAT4, inTransform)
  REGISTER_PARAMETER_MACRO("inOffset", ANARI_FLOAT32_VEC4, inOffset)
  REGISTER_PARAMETER_MACRO("outTransform", ANARI_FLOAT32_MAT4, outTransform)
  REGISTER_PARAMETER_MACRO("outOffset", ANARI_FLOAT32_VEC4, outOffset)
)

constexpr UsdSampler::ComponentPair UsdSampler::componentParamNames[]; // Workaround for C++14's lack of inlining constexpr arrays

namespace
{ 
  UsdBridgeSamplerData::WrapMode ANARIToUsdBridgeWrapMode(const char* anariWrapMode)
  {
    UsdBridgeSamplerData::WrapMode usdWrapMode = UsdBridgeSamplerData::WrapMode::BLACK;
    if(anariWrapMode)
    {
      if (strEquals(anariWrapMode, "clampToEdge"))
      {
        usdWrapMode = UsdBridgeSamplerData::WrapMode::CLAMP;
      }
      else if (strEquals(anariWrapMode, "repeat"))
      {
        usdWrapMode = UsdBridgeSamplerData::WrapMode::REPEAT;
      }
      else if (strEquals(anariWrapMode, "mirrorRepeat"))
      {
        usdWrapMode = UsdBridgeSamplerData::WrapMode::MIRROR;
      }
    }
    return usdWrapMode;
  }
}

UsdSampler::UsdSampler(const char* name, const char* type, UsdDevice* device)
  : BridgedBaseObjectType(ANARI_SAMPLER, name, device)
{
  if (strEquals(type, "image1D"))
    samplerType = SAMPLER_1D;
  else if (strEquals(type, "image2D"))
    samplerType = SAMPLER_2D;
  else if (strEquals(type, "transform"))
    samplerType = SAMPLER_TRANSFORM;
  else if (strEquals(type, "image3D"))
    samplerType = SAMPLER_3D;
  else
    device->reportStatus(this, ANARI_SAMPLER, ANARI_SEVERITY_ERROR, ANARI_STATUS_INVALID_ARGUMENT, "UsdSampler '%s' construction failed: type %s not supported", getName(), type);
}

UsdSampler::~UsdSampler()
{
#ifdef OBJECT_LIFETIME_EQUALS_USD_LIFETIME
  if(cachedBridge)
    cachedBridge->DeleteSampler(usdHandle);
#endif
}

void UsdSampler::remove(UsdDevice* device)
{
  applyRemoveFunc(device, &UsdBridge::DeleteSampler);
}

void UsdSampler::updateBoundParameters(bool boundToInstance, const UsdSharedString*const* attribNames, size_t numAttribNames, UsdDevice* device) 
{ 
  UsdBridge* usdBridge = device->getUsdBridge();

  if(!usdHandle.value)
    return;
  
  perInstance = boundToInstance;

  if(hasBakedCoordinates()) // Reads a primvar of its own, whatever the geometry is
    return;

  UsdLogInfo logInfo(device, this, ANARI_SAMPLER, getName());

  // Fix up the position attribute
  const UsdSamplerData& paramData = getReadParams();
  const char* inAttribName = UsdSharedString::c_str(paramData.inAttribute);
  
  auto [hasMeshDependentAttribs, meshDependentAttribName] = 
    GetGeomDependentAttributeName(inAttribName, perInstance, device->getReadParams().useDisplayColorOpacity, attribNames, numAttribNames, logInfo);

  if(hasMeshDependentAttribs)
  {
    double worldTimeStep = device->getReadParams().timeStep;
    double dataTimeStep = selectObjTime(paramData.timeStep, worldTimeStep);

    UsdBridgeSamplerData::DataMemberId timeVarying;
    setSamplerTimeVarying(timeVarying);
    
    usdBridge->ChangeInAttribute(usdHandle, meshDependentAttribName, dataTimeStep, timeVarying);
  } 
}

bool UsdSampler::deferCommit(UsdDevice* device)
{
  return false;
}

namespace
{
  // Index of "attributeN", or -1
  int attributeIndexOf(const char* attribName)
  {
    if(attribName && !std::strncmp(attribName, "attribute", 9) && attribName[9] >= '0' && attribName[9] <= '9' && attribName[10] == '\0')
      return attribName[9] - '0';
    return -1;
  }
}

const char* UsdSampler::getScalarOutputName() const
{
  const std::string outputName = std::string(getName()) + "_r";
  if(outputName != scalarOutputName) // Pointers to the name stay valid as long as it does not change
    scalarOutputName = outputName;
  return scalarOutputName.c_str();
}

int UsdSampler::getTexCoordAttribute() const
{
  if(samplerType != SAMPLER_2D || hasBakedCoordinates())
    return -1;
  const char* attribName = UsdSharedString::c_str(getReadParams().inAttribute);
  return attribName ? attributeIndexOf(attribName) : 0;
}

UsdSampler::CoordTransform UsdSampler::getCoordTransform(float scale[2], float& rotation, float translation[2]) const
{
  scale[0] = scale[1] = 1.0f;
  rotation = 0.0f;
  translation[0] = translation[1] = 0.0f;
  if(samplerType != SAMPLER_2D)
    return COORDS_AS_IS;

  const UsdSamplerData& paramData = getReadParams();
  const float* m = paramData.inTransform.Data; // Column-major
  const float* o = paramData.inOffset.Data;
  const char* attribName = UsdSharedString::c_str(paramData.inAttribute);

  // Only the first two rows end up in the coordinates of a 2D sampler: the plane is
  // (m0 m4; m1 m5), with a fourth coordinate of one the translation is column 3 plus the offset
  const float eps = 1e-5f;
  const float pi = 3.14159265358979f;
  const float scaleX = std::sqrt(m[0]*m[0] + m[1]*m[1]);
  const float lengthY = std::sqrt(m[4]*m[4] + m[5]*m[5]);
  const float angle = scaleX > eps ? std::atan2(m[1], m[0]) : std::atan2(-m[4], m[5]);
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  const float shear = m[4]*c + m[5]*s;

  scale[0] = scaleX;
  scale[1] = -m[4]*s + m[5]*c;
  rotation = angle * 180.0f / pi;
  translation[0] = m[12] + o[0];
  translation[1] = m[13] + o[1];

  // Positions are no primvar a reader of texture coordinates can take
  const bool readsPositions = attribName && strEquals(attribName, "objectPosition");
  const bool usesThirdCoord = std::abs(m[8]) > eps || std::abs(m[9]) > eps;
  if(readsPositions || usesThirdCoord || std::abs(shear) > eps * std::max(1.0f, lengthY))
    return COORDS_BAKED;

  const bool asIs = std::abs(scale[0] - 1.0f) <= eps && std::abs(scale[1] - 1.0f) <= eps && std::abs(angle) <= eps
    && std::abs(translation[0]) <= eps && std::abs(translation[1]) <= eps;
  return asIs ? COORDS_AS_IS : COORDS_TRANSFORMED;
}

bool UsdSampler::hasBakedCoordinates() const
{
  float scale[2], rotation, translation[2];
  return getCoordTransform(scale, rotation, translation) == COORDS_BAKED;
}

const char* UsdSampler::getBakedCoordinatesName() const
{
  const std::string coordName = std::string(getName()) + "_st";
  if(coordName != bakedCoordinatesName) // Pointers to the name stay valid as long as it does not change
    bakedCoordinatesName = coordName;
  return bakedCoordinatesName.c_str();
}

void UsdSampler::bakeCoordinates(UsdGeometry* geometry, UsdDevice* device) const
{
  if(!hasBakedCoordinates())
    return;
  const UsdSamplerData& paramData = getReadParams();
  bakePrimvar(geometry, paramData.inTransform.Data, paramData.inOffset.Data, 2, getBakedCoordinatesName(), device);
}

void UsdSampler::bakeAttribute(UsdGeometry* geometry, bool scalar, UsdDevice* device) const
{
  const UsdSamplerData& paramData = getReadParams();
  bakePrimvar(geometry, paramData.outTransform.Data, paramData.outOffset.Data, scalar ? 1 : 3,
    scalar ? getScalarOutputName() : getName(), device);
}

void UsdSampler::bakePrimvar(UsdGeometry* geometry, const float* m, const float* o,
  int numComponents, const char* primvarName, UsdDevice* device) const
{
  const UsdSamplerData& paramData = getReadParams();
  const UsdGeometryData& geomData = geometry->getReadParams();
  const char* attribName = UsdSharedString::c_str(paramData.inAttribute);

  const UsdDataArray* source = nullptr;
  const UsdDataArray* perPrimSource = nullptr;
  const int attribIndex = attribName ? attributeIndexOf(attribName) : 0;
  if(attribName && strEquals(attribName, "color"))
  {
    source = geomData.vertexColors;
    perPrimSource = geomData.primitiveColors;
  }
  else if(attribName && strEquals(attribName, "objectPosition"))
  {
    source = geomData.vertexPositions;
  }
  else if(attribIndex >= 0 && attribIndex < MAX_ATTRIBS)
  {
    source = geomData.vertexAttributes[attribIndex];
    perPrimSource = geomData.primitiveAttributes[attribIndex];
  }

  const bool perPrim = !source;
  if(!source)
    source = perPrimSource;

  // An attribute which the geometry does not have is (0, 0, 0, 1) everywhere
  const bool hasSource = source && source->getData() && source->getLayout().isDense();
  const size_t numElements = hasSource ? source->getLayout().numItems1 : 1;

  std::vector<float> values(numElements * numComponents);
  for(size_t i = 0; i < numElements; ++i)
  {
    float in[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if(hasSource)
    {
      const auto value = helium::readAsAttributeValueFlat(source->getData(), source->getType(), i);
      in[0] = value.x; in[1] = value.y; in[2] = value.z; in[3] = value.w;
    }
    for(int row = 0; row < numComponents; ++row)
      values[i*numComponents + row] = m[row]*in[0] + m[4+row]*in[1] + m[8+row]*in[2] + m[12+row]*in[3] + o[row];
  }

  const UsdBridgePrimvarScope scope = !hasSource ? UsdBridgePrimvarScope::CONSTANT
    : (perPrim ? UsdBridgePrimvarScope::PRIMITIVE : UsdBridgePrimvarScope::VERTEX);
  device->getUsdBridge()->SetGeometryPrimvar(geometry->getUsdHandle(),
    primvarName, values.data(), numElements, numComponents, scope);
}

// A UsdUVTexture has a scale and a bias per channel, an ANARI sampler a matrix and an offset
// for its output. What the matrix does beyond a scale is applied to the texels here, which
// gives the same result as applying it to the filtered texel: both are linear. The texels are
// written sRGB encoded, with eight bits per channel; a result above one is brought into that
// range by a factor which becomes the scale of the texture.
bool UsdSampler::bakeOutputTransform(const UsdSamplerData& paramData, UsdBridgeSamplerData& samplerData)
{
  static const UsdFloatMat4 identity;
  const float* m = paramData.outTransform.Data; // Column-major
  const float* o = paramData.outOffset.Data;

  const bool noTransform = std::equal(m, m + 16, identity.Data)
    && o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f && o[3] == 0.0f;
  const UsdDataArray* image = paramData.imageData;
  const UsdDataLayout& layout = image->getLayout();
  if(noTransform || !image->getData() || !layout.isDense() || layout.numItems3 > 1)
    return false;

  const ANARIDataType type = image->getType();
  const size_t numTexels = layout.numItems1 * layout.numItems2;
  const int numComponents = std::min(static_cast<int>(anari::componentsOf(type)), 4);
  const int numColors = numComponents == 2 ? 1 : std::min(numComponents, 3); // The rest is alpha

  std::vector<float> result(numTexels * 4);
  float maxColor = 1.0f;
  for(size_t i = 0; i < numTexels; ++i)
  {
    const auto texel = helium::readAsAttributeValueFlat(image->getData(), type, i);
    float* out = &result[i*4];
    for(int row = 0; row < 4; ++row)
      out[row] = m[row]*texel.x + m[4+row]*texel.y + m[8+row]*texel.z + m[12+row]*texel.w + o[row];
    for(int c = 0; c < numColors; ++c)
      maxColor = std::max(maxColor, out[c]);
  }

  auto encodeSrgb = [](float linear) -> unsigned char
  {
    linear = std::min(std::max(linear, 0.0f), 1.0f);
    const float encoded = linear <= 0.0031308f ? 12.92f*linear : 1.055f*std::pow(linear, 1.0f/2.4f) - 0.055f;
    return static_cast<unsigned char>(encoded*255.0f + 0.5f);
  };

  bakedImage.resize(numTexels * numComponents);
  for(size_t i = 0; i < numTexels; ++i)
  {
    const float* in = &result[i*4];
    unsigned char* out = &bakedImage[i*numComponents];
    for(int c = 0; c < numColors; ++c)
      out[c] = encodeSrgb(in[c] / maxColor);
    if(numColors < numComponents)
      out[numColors] = static_cast<unsigned char>(std::min(std::max(in[3], 0.0f), 1.0f)*255.0f + 0.5f);
  }

  static const UsdBridgeType srgbTypes[] = {
    UsdBridgeType::UCHAR_SRGB_R, UsdBridgeType::UCHAR_SRGB_RA, UsdBridgeType::UCHAR_SRGB_RGB, UsdBridgeType::UCHAR_SRGB_RGBA};

  samplerData.Data = bakedImage.data();
  samplerData.DataType = srgbTypes[numComponents-1];
  samplerData.ImageName = nullptr; // The texels are those of this sampler only
  samplerData.OutputScale = maxColor;
  samplerData.ImageStride[0] = numComponents;
  samplerData.ImageStride[1] = static_cast<int64_t>(layout.numItems1) * numComponents;
  samplerData.ImageStride[2] = static_cast<int64_t>(numTexels) * numComponents;

  return true;
}

bool UsdSampler::doCommitData(UsdDevice* device)
{
  UsdBridge* usdBridge = device->getUsdBridge();

  if(!device->getReadParams().outputMaterial ||
    samplerType == SAMPLER_UNKNOWN ||
    samplerType == SAMPLER_TRANSFORM) // Written with the geometries it is used on
    return false;

  const UsdSamplerData& paramData = getReadParams();

  UsdBridgeSamplerData::SamplerType type = 
    (samplerType == SAMPLER_1D ? UsdBridgeSamplerData::SamplerType::SAMPLER_1D : 
      (samplerType == SAMPLER_2D ? UsdBridgeSamplerData::SamplerType::SAMPLER_2D :
        UsdBridgeSamplerData::SamplerType::SAMPLER_3D
        )
      );

  bool isNew = false;
  if (!usdHandle.value)
    isNew = usdBridge->CreateSampler(getName(), usdHandle, type);

  if (paramChanged || isNew)
  {
    if (paramData.inAttribute && (std::strlen(UsdSharedString::c_str(paramData.inAttribute)) > 0) 
      && (paramData.imageUrl || paramData.imageData))
    {
      bool supportedImage = true;
      int numComponents = 0;
      if(paramData.imageData)
      {
        numComponents = static_cast<int>(anari::componentsOf(paramData.imageData->getType()));

        if(numComponents > 4)
          device->reportStatus(this, ANARI_SAMPLER, ANARI_SEVERITY_WARNING, ANARI_STATUS_INVALID_ARGUMENT, 
            "UsdSampler '%s' image data has more than 4 components. Anything above the 4th component will be ignored.", paramData.imageData->getName());
      }

      if(supportedImage)
      {
        UsdLogInfo logInfo(device, this, ANARI_SAMPLER, getName());

        UsdBridgeSamplerData samplerData;
        samplerData.Type = type;

        double worldTimeStep = device->getReadParams().timeStep;
        double dataTimeStep = selectObjTime(paramData.timeStep, worldTimeStep);

        samplerData.InAttribute = AnariAttributeToUsdName(UsdSharedString::c_str(paramData.inAttribute), perInstance, device->getReadParams().useDisplayColorOpacity, logInfo);

        const CoordTransform coordTransform = getCoordTransform(
          samplerData.CoordScale, samplerData.CoordRotation, samplerData.CoordTranslation);
        samplerData.HasCoordTransform = coordTransform == COORDS_TRANSFORMED;
        if(coordTransform == COORDS_BAKED)
          samplerData.InAttribute = getBakedCoordinatesName();
      
        if(paramData.imageUrl)
        {
          samplerData.ImageUrl = UsdSharedString::c_str(paramData.imageUrl);
        }

        if(paramData.imageData)
        {
          samplerData.Data = paramData.imageData->getData();
          samplerData.ImageName = paramData.imageData->getName();
          samplerData.ImageNumComponents = numComponents;
          samplerData.DataType = AnariToUsdBridgeType(paramData.imageData->getType());
          paramData.imageData->getLayout().copyDims(samplerData.ImageDims);
          paramData.imageData->getLayout().copyStride(samplerData.ImageStride);

          bakeOutputTransform(paramData, samplerData);
        }

        samplerData.WrapS = ANARIToUsdBridgeWrapMode(UsdSharedString::c_str(paramData.wrapS));
        samplerData.WrapT = ANARIToUsdBridgeWrapMode(UsdSharedString::c_str(paramData.wrapT));
        samplerData.WrapR = ANARIToUsdBridgeWrapMode(UsdSharedString::c_str(paramData.wrapR));

        setSamplerTimeVarying(samplerData.TimeVarying);

        usdBridge->SetSamplerData(usdHandle, samplerData, dataTimeStep);
      }
    }
    else
    {
      device->reportStatus(this, ANARI_SAMPLER, ANARI_SEVERITY_ERROR, ANARI_STATUS_INVALID_ARGUMENT, 
        "UsdSampler '%s' commit failed: missing either the 'inAttribute', or both the 'image' and 'usd::imageUrl' parameter", getName());
    }

    paramChanged = false;
  }

  return false;
}

void UsdSampler::setSamplerTimeVarying(UsdBridgeSamplerData::DataMemberId& timeVarying)
{
  typedef UsdBridgeSamplerData::DataMemberId DMI;

  timeVarying = DMI::ALL
    & (isTimeVarying(CType::DATA) ? DMI::ALL : ~DMI::DATA)
    & (isTimeVarying(CType::WRAPS) ? DMI::ALL : ~DMI::WRAPS)
    & (isTimeVarying(CType::WRAPT) ? DMI::ALL : ~DMI::WRAPT)
    & (isTimeVarying(CType::WRAPR) ? DMI::ALL : ~DMI::WRAPR);
}
