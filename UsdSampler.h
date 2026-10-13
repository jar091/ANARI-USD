// Copyright 2020 The Khronos Group
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "UsdBridgedBaseObject.h"

#include <string>

enum class UsdSamplerComponents
{
  DATA = 0,
  WRAPS,
  WRAPT,
  WRAPR
};

struct UsdSamplerData
{
  UsdSharedString* name = nullptr;
  UsdSharedString* usdName = nullptr;

  double timeStep = std::numeric_limits<float>::quiet_NaN();
  int timeVarying = 0; // Bitmask indicating which attributes are time-varying.

  const UsdDataArray* imageData = nullptr;

  UsdSharedString* inAttribute = nullptr; 
  UsdSharedString* imageUrl = nullptr;
  UsdSharedString* wrapS = nullptr;
  UsdSharedString* wrapT = nullptr;
  UsdSharedString* wrapR = nullptr;

  UsdFloatMat4 inTransform;
  UsdFloat4 inOffset = {0.0f, 0.0f, 0.0f, 0.0f};
  UsdFloatMat4 outTransform;
  UsdFloat4 outOffset = {0.0f, 0.0f, 0.0f, 0.0f};
};

class UsdGeometry;

class UsdSampler : public UsdBridgedBaseObject<UsdSampler, UsdSamplerData, UsdSamplerHandle, UsdSamplerComponents>
{
  protected:
    enum SamplerType
    {
      SAMPLER_UNKNOWN = 0,
      SAMPLER_1D,
      SAMPLER_2D,
      SAMPLER_3D,
      SAMPLER_TRANSFORM
    };

  public:
    UsdSampler(const char* name, const char* type, UsdDevice* device);
    ~UsdSampler();

    void remove(UsdDevice* device) override;

    bool isPerInstance() const { return perInstance; }

    // A transform sampler has no image: its output is its attribute, transformed. A preview
    // surface cannot transform what it reads from a primvar, so the output is computed for
    // every geometry which the sampler is used on, as a primvar named after the sampler.
    // An input of one float reads a primvar of floats, which has a name of its own.
    bool isTransform() const { return samplerType == SAMPLER_TRANSFORM; }
    const char* getScalarOutputName() const;
    void bakeAttribute(UsdGeometry* geometry, bool scalar, UsdDevice* device) const;

    // Index of the attribute which an image sampler takes its texture coordinates from, or -1
    int getTexCoordAttribute() const;

    // What inTransform and inOffset do to the texture coordinates of an image sampler. A scale,
    // rotation and translation in the plane is a node of the shading network. Anything else is
    // computed for every geometry which the sampler is used on, as texture coordinates named
    // after the sampler.
    enum CoordTransform
    {
      COORDS_AS_IS = 0,
      COORDS_TRANSFORMED,
      COORDS_BAKED
    };
    CoordTransform getCoordTransform(float scale[2], float& rotation, float translation[2]) const;
    bool hasBakedCoordinates() const;
    const char* getBakedCoordinatesName() const;
    void bakeCoordinates(UsdGeometry* geometry, UsdDevice* device) const;

    // The sampler of the opacity of a material writes that opacity (the first component of
    // what it returns) into the alpha channel of its image as well. An opacity connected to
    // the alpha of a texture is what readers take as the coverage of the surface, which is
    // what it means in ANARI; connected to a color channel it is commonly taken as the
    // translucency of glass. Set by the material before the samplers are written.
    void setOpacityInAlpha(bool enable) { opacityInAlpha = enable; }
    bool writesOpacityToAlpha() const;

    void updateBoundParameters(bool boundToInstance, const UsdSharedString*const* attribNames, size_t numAttribNames, UsdDevice* device);

    static constexpr ComponentPair componentParamNames[] = {
      ComponentPair(CType::DATA, "image"),
      ComponentPair(CType::DATA, "imageUrl"),
      ComponentPair(CType::WRAPS, "wrapMode"),
      ComponentPair(CType::WRAPS, "wrapMode1"),
      ComponentPair(CType::WRAPT, "wrapMode2"),
      ComponentPair(CType::WRAPR, "wrapMode3")};

  protected:
    bool deferCommit(UsdDevice* device) override;
    bool doCommitData(UsdDevice* device) override;
    void doCommitRefs(UsdDevice* device) override {}

    void setSamplerTimeVarying(UsdBridgeSamplerData::DataMemberId& timeVarying);

    SamplerType samplerType = SAMPLER_UNKNOWN;

    // Applies outTransform and outOffset to the texels, returns whether samplerData has been changed
    bool bakeOutputTransform(const UsdSamplerData& paramData, UsdBridgeSamplerData& samplerData);

    std::vector<unsigned char> bakedImage;
    mutable std::string scalarOutputName;
    mutable std::string bakedCoordinatesName;

    bool opacityInAlpha = false;

    // Writes transform*attribute + offset as a primvar of the geometry
    void bakePrimvar(UsdGeometry* geometry, const float* transform, const float* offset,
      int numComponents, const char* primvarName, UsdDevice* device) const;

    bool perInstance = false; // Whether sampler is attached to a point instancer
    bool instanceAttributeAttached = false; // Whether a value to inAttribute has been set which in USD is different between per-instance and regular geometries
};