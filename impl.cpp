#include "impl.h"

#include <xinput.h>
#include <array>
#include <cstring>

#include "ShaderSampleRateConverter.h"
#include "util.h"

namespace atfix {
static mutex  g_globalMutex;

/** Metadata */
static const GUID IID_StagingShadowResource = {0xe2728d91,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

static const GUID IID_MSAACandidate = {0xe2728d93,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_MSAATexture = {0xe2728d94,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_AlphaToCoverage = {0xe2728d95,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_OriginalShader = {0xe2728d96,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_DeviceWrapper = {0xe2728d97,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};

enum class MSAACandidateState : UINT {
  None = 0,
  Possible, // Created SRV, not shadow buffer
  Probable, // Likely to be the main RV
  Dirty,    // MSAA texture created, not yet resolved
  Clean,    // MSAA texture created, has been resolved
};
// Static const versions that we can take pointers to
static const MSAACandidateState MSAACandidateStatePossible = MSAACandidateState::Possible;
static const MSAACandidateState MSAACandidateStateProbable = MSAACandidateState::Probable;
static const MSAACandidateState MSAACandidateStateDirty = MSAACandidateState::Dirty;
static const MSAACandidateState MSAACandidateStateClean = MSAACandidateState::Clean;

struct ATFIX_RESOURCE_INFO {
  D3D11_RESOURCE_DIMENSION Dim;
  DXGI_FORMAT Format;
  uint32_t Width;
  uint32_t Height;
  uint32_t Depth;
  uint32_t Layers;
  uint32_t Mips;
  D3D11_USAGE Usage;
  uint32_t BindFlags;
  uint32_t MiscFlags;
  uint32_t CPUFlags;
};

static decltype(XInputGetState)* loadXInput() {
  static HMODULE mod = LoadLibraryA("Xinput1_3.dll");
  if (!mod)
    return nullptr;
  return reinterpret_cast<decltype(XInputGetState)*>(GetProcAddress(mod, "XInputGetState"));
}

static bool shouldUseOldShaders() {
  if (!config.allowShaderToggle)
    return false;
  static decltype(XInputGetState)* getState = loadXInput();
  XINPUT_STATE state;
  if (getState && SUCCEEDED(getState(0, &state)))
    return state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK;
  return false;
}

static bool isPowerOfTwo(UINT value) {
  return (value & (value - 1)) == 0;
}

void* ptroffset(void* base, ptrdiff_t offset) {
  auto address = reinterpret_cast<uintptr_t>(base) + offset;
  return reinterpret_cast<void*>(address);
}

uint32_t getFormatPixelSize(
        DXGI_FORMAT               Format) {
  struct FormatRange {
    DXGI_FORMAT MinFormat;
    DXGI_FORMAT MaxFormat;
    uint32_t FormatSize;
  };

  static const std::array<FormatRange, 7> s_ranges = {{
    { DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_SINT,    16u },
    { DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_SINT,       12u },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R32G32_SINT,          8u  },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R32_SINT,             4u  },
    { DXGI_FORMAT_B8G8R8A8_UNORM,         DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,  4u  },
    { DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R16_SINT,             2u  },
    { DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_A8_UNORM,             1u  },
  }};

  for (const auto& range : s_ranges) {
    if (Format >= range.MinFormat && Format <= range.MaxFormat)
      return range.FormatSize;
  }

  log("Unhandled format ", Format);
  return 1u;
}

bool getResourceInfo(
        ID3D11Resource*           pResource,
        ATFIX_RESOURCE_INFO*      pInfo) {
  pResource->GetType(&pInfo->Dim);

  switch (pInfo->Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      pInfo->Format = DXGI_FORMAT_UNKNOWN;
      pInfo->Width = desc.ByteWidth;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = 1;
      pInfo->Mips = 1;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = 1;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = 1;
      pInfo->Layers = desc.ArraySize;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      pInfo->Format = desc.Format;
      pInfo->Width = desc.Width;
      pInfo->Height = desc.Height;
      pInfo->Depth = desc.Depth;
      pInfo->Layers = 1;
      pInfo->Mips = desc.MipLevels;
      pInfo->Usage = desc.Usage;
      pInfo->BindFlags = desc.BindFlags;
      pInfo->MiscFlags = desc.MiscFlags;
      pInfo->CPUFlags = desc.CPUAccessFlags;
    } return true;

    default:
      log("Unhandled resource dimension ", pInfo->Dim);
      return false;
  }
}

D3D11_BOX getResourceBox(
  const ATFIX_RESOURCE_INFO*      pInfo,
        UINT                      Subresource) {
  uint32_t mip = Subresource % pInfo->Mips;

  uint32_t w = std::max(pInfo->Width >> mip, 1u);
  uint32_t h = std::max(pInfo->Height >> mip, 1u);
  uint32_t d = std::max(pInfo->Depth >> mip, 1u);

  return D3D11_BOX { 0, 0, 0, w, h, d };
}

bool isImmediatecontext(
        ID3D11DeviceContext*      pContext) {
  return pContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

bool isCpuWritableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING || pInfo->Usage == D3D11_USAGE_DYNAMIC)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_WRITE)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

bool isCpuReadableResource(
  const ATFIX_RESOURCE_INFO*      pInfo) {
  return (pInfo->Usage == D3D11_USAGE_STAGING)
      && (pInfo->CPUFlags & D3D11_CPU_ACCESS_READ)
      && (pInfo->Layers == 1)
      && (pInfo->Mips == 1);
}

ID3D11Resource* getMSAATexture(ID3D11Resource* host) {
  ID3D11Resource* res = nullptr;
  UINT size = sizeof(res);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAATexture, &size, &res)))
    return res;
  return nullptr;
}

ID3D11RenderTargetView* getMSAARTV(ID3D11RenderTargetView* host) {
  ID3D11RenderTargetView* res = nullptr;
  UINT size = sizeof(res);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAATexture, &size, &res)))
    return res;
  return nullptr;
}

ID3D11DepthStencilView* getMSAADSV(ID3D11DepthStencilView* host) {
  ID3D11DepthStencilView* res = nullptr;
  UINT size = sizeof(res);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAATexture, &size, &res)))
    return res;
  return nullptr;
}

ID3D11RenderTargetView* getOrCreateMSAARTV(ID3D11Device* dev, ID3D11RenderTargetView* host) {
  ID3D11RenderTargetView* rtv = nullptr;
  UINT size = sizeof(rtv);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAATexture, &size, &rtv)))
    return rtv;
  ID3D11Texture2D* tex = nullptr;
  size = sizeof(tex);
  ID3D11Resource* resource;
  D3D11_RENDER_TARGET_VIEW_DESC vdesc;
  host->GetDesc(&vdesc);
  host->GetResource(&resource);
  if (FAILED(resource->GetPrivateData(IID_MSAATexture, &size, &tex))) {
    ID3D11Texture2D* hostTex = nullptr;
    resource->QueryInterface(IID_PPV_ARGS(&hostTex));
    D3D11_TEXTURE2D_DESC desc;
    hostTex->GetDesc(&desc);
    desc.Format = vdesc.Format;
    desc.SampleDesc.Count = config.msaaSamples;
    desc.SampleDesc.Quality = 0;
    while (desc.SampleDesc.Count > 1) {
      UINT quality = 0;
      if (SUCCEEDED(dev->CheckMultisampleQualityLevels(desc.Format, desc.SampleDesc.Count, &quality)) && quality > 0)
        break;
      desc.SampleDesc.Count /= 2;
    }
    log("Creating ", std::dec, desc.Width, "x", desc.Height, " MSAA color texture with format ", desc.Format, "...");
    dev->CreateTexture2D(&desc, nullptr, &tex);
    resource->SetPrivateDataInterface(IID_MSAATexture, tex);
  }
  resource->Release();
  vdesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
  dev->CreateRenderTargetView(tex, &vdesc, &rtv);
  host->SetPrivateDataInterface(IID_MSAATexture, rtv);
  tex->Release();
  return rtv;
}

ID3D11DepthStencilView* getOrCreateMSAADSV(ID3D11Device* dev, ID3D11DepthStencilView* host) {
  ID3D11DepthStencilView* dsv = nullptr;
  UINT size = sizeof(dsv);
  if (SUCCEEDED(host->GetPrivateData(IID_MSAATexture, &size, &dsv)))
    return dsv;
  ID3D11Texture2D* tex = nullptr;
  size = sizeof(tex);
  ID3D11Resource* resource;
  D3D11_DEPTH_STENCIL_VIEW_DESC vdesc;
  host->GetResource(&resource);
  host->GetDesc(&vdesc);
  if (FAILED(resource->GetPrivateData(IID_MSAATexture, &size, &tex))) {
    ID3D11Texture2D* hostTex = nullptr;
    resource->QueryInterface(IID_PPV_ARGS(&hostTex));
    D3D11_TEXTURE2D_DESC desc;
    hostTex->GetDesc(&desc);
    desc.Format = vdesc.Format;
    desc.SampleDesc.Count = config.msaaSamples;
    desc.SampleDesc.Quality = 0;
    desc.BindFlags &= ~D3D11_BIND_SHADER_RESOURCE;
    while (desc.SampleDesc.Count > 1) {
      UINT quality = 0;
      if (SUCCEEDED(dev->CheckMultisampleQualityLevels(desc.Format, desc.SampleDesc.Count, &quality)) && quality > 0)
        break;
      desc.SampleDesc.Count /= 2;
    }
    log("Creating ", std::dec, desc.Width, "x", desc.Height, " MSAA depth texture with format ", desc.Format, "...");
    dev->CreateTexture2D(&desc, nullptr, &tex);
    resource->SetPrivateDataInterface(IID_MSAATexture, tex);
  }
  resource->Release();
  vdesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
  dev->CreateDepthStencilView(tex, &vdesc, &dsv);
  host->SetPrivateDataInterface(IID_MSAATexture, dsv);
  tex->Release();
  return dsv;
}

void resolveIfMSAA(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
  if (ID3D11Resource* msaa = getMSAATexture(res)) {
    MSAACandidateState state = MSAACandidateState::None;
    UINT size = sizeof(state);
    if (SUCCEEDED(res->GetPrivateData(IID_MSAACandidate, &size, &state)) && state == MSAACandidateState::Dirty) {
      ID3D11Texture2D* tex;
      D3D11_TEXTURE2D_DESC desc;
      msaa->QueryInterface(IID_PPV_ARGS(&tex));
      tex->GetDesc(&desc);
      tex->Release();
      ctx->ResolveSubresource(res, 0, msaa, 0, desc.Format);
      res->SetPrivateData(IID_MSAACandidate, sizeof(state), &MSAACandidateStateClean);
    }
    msaa->Release();
  }
}

ID3D11Resource* createShadowResourceLocked(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  ID3D11Device* device = nullptr;
  pContext->GetDevice(&device);

  ATFIX_RESOURCE_INFO resourceInfo = { };
  getResourceInfo(pBaseResource, &resourceInfo);

  ID3D11Resource* shadowResource = nullptr;
  HRESULT hr;

  switch (resourceInfo.Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&buffer));

      D3D11_BUFFER_DESC desc = { };
      buffer->GetDesc(&desc);
      buffer->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
      desc.StructureByteStride = 0;

      ID3D11Buffer* shadowBuffer = nullptr;
      hr = device->CreateBuffer(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      ID3D11Texture1D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE1D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture1D* shadowBuffer = nullptr;
      hr = device->CreateTexture1D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      ID3D11Texture2D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE2D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture2D* shadowBuffer = nullptr;
      hr = device->CreateTexture2D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      ID3D11Texture3D* texture = nullptr;
      pBaseResource->QueryInterface(IID_PPV_ARGS(&texture));

      D3D11_TEXTURE3D_DESC desc = { };
      texture->GetDesc(&desc);
      texture->Release();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.MiscFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

      ID3D11Texture3D* shadowBuffer = nullptr;
      hr = device->CreateTexture3D(&desc, nullptr, &shadowBuffer);

      shadowResource = shadowBuffer;
    } break;

    default:
      log("Unhandled resource dimension ", resourceInfo.Dim);
      hr = E_INVALIDARG;
  }

  if (SUCCEEDED(hr)) {
    pContext->CopyResource(shadowResource, pBaseResource);
    pBaseResource->SetPrivateDataInterface(IID_StagingShadowResource, shadowResource);
  } else
    log("Failed to create shadow resource, hr ", std::hex, hr);

  device->Release();
  return shadowResource;
}

ID3D11Resource* getShadowResourceLocked(
        ID3D11Resource*           pBaseResource) {
  ID3D11Resource* shadowResource = nullptr;
  UINT resultSize = sizeof(shadowResource);
  
  if (SUCCEEDED(pBaseResource->GetPrivateData(IID_StagingShadowResource, &resultSize, &shadowResource)))
    return shadowResource;

  return nullptr;
}

ID3D11Resource* getShadowResource(
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  return getShadowResourceLocked(pBaseResource);
}

ID3D11Resource* getOrCreateShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pBaseResource) {
  std::lock_guard lock(g_globalMutex);
  ID3D11Resource* shadowResource = getShadowResourceLocked(pBaseResource);

  if (!shadowResource)
    shadowResource = createShadowResourceLocked(pContext, pBaseResource);

  return shadowResource;
}

void updateViewShadowResource(
        ID3D11DeviceContext*      pContext,
        ID3D11View*               pView) {

  ID3D11Resource* baseResource;
  pView->GetResource(&baseResource);

  ID3D11Resource* shadowResource = getShadowResource(baseResource);

  if (shadowResource) {
    ATFIX_RESOURCE_INFO resourceInfo = { };
    getResourceInfo(baseResource, &resourceInfo);

    uint32_t mipLevel = 0;
    uint32_t layerIndex = 0;
    uint32_t layerCount = 1;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;

    if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&rtv)))) {
      D3D11_RENDER_TARGET_VIEW_DESC desc = { };
      rtv->GetDesc(&desc);
      rtv->Release();

      switch (desc.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_RTV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled RTV dimension ", desc.ViewDimension);
      }
    } else if (SUCCEEDED(pView->QueryInterface(IID_PPV_ARGS(&uav)))) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc = { };
      uav->GetDesc(&desc);
      uav->Release();

      switch (desc.ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER:
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1D:
          mipLevel = desc.Texture1D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
          mipLevel = desc.Texture1DArray.MipSlice;
          layerIndex = desc.Texture1DArray.FirstArraySlice;
          layerCount = desc.Texture1DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2D:
          mipLevel = desc.Texture2D.MipSlice;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          mipLevel = desc.Texture2DArray.MipSlice;
          layerIndex = desc.Texture2DArray.FirstArraySlice;
          layerCount = desc.Texture2DArray.ArraySize;
          break;

        case D3D11_UAV_DIMENSION_TEXTURE3D:
          mipLevel = desc.Texture3D.MipSlice;
          break;

        default:
          log("Unhandled UAV dimension ", desc.ViewDimension);
      }
    } else {
      log("Unhandled view type");
    }

    for (uint32_t i = 0; i < layerCount; i++) {
      uint32_t subresource = D3D11CalcSubresource(mipLevel, layerIndex + i, resourceInfo.Mips);

      pContext->CopySubresourceRegion(
        shadowResource, subresource, 0, 0, 0,
        baseResource,   subresource, nullptr);
    }

    shadowResource->Release();
  }

  baseResource->Release();
}

void updateRtvShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11RenderTargetView*, 8> rtvs;
  pContext->OMGetRenderTargets(rtvs.size(), rtvs.data(), nullptr);

  for (ID3D11RenderTargetView* rtv : rtvs) {
    if (rtv) {
      updateViewShadowResource(pContext, rtv);
      rtv->Release();
    }
  }
}

void updateUavShadowResources(
        ID3D11DeviceContext*      pContext) {
  std::array<ID3D11UnorderedAccessView*, 8> uavs;
  pContext->CSGetUnorderedAccessViews(0, uavs.size(), uavs.data());

  for (ID3D11UnorderedAccessView* uav : uavs) {
    if (uav) {
      updateViewShadowResource(pContext, uav);
      uav->Release();
    }
  }
}

HRESULT tryCpuCopy(
        ID3D11DeviceContext*      pContext,
        ID3D11Resource*           pDstResource,
        UINT                      DstSubresource,
        UINT                      DstX,
        UINT                      DstY,
        UINT                      DstZ,
        ID3D11Resource*           pSrcResource,
        UINT                      SrcSubresource,
  const D3D11_BOX*                pSrcBox) {
  ATFIX_RESOURCE_INFO dstInfo = { };
  getResourceInfo(pDstResource, &dstInfo);

  if (!isCpuWritableResource(&dstInfo))
    return E_INVALIDARG;

  /* Compute source region for the given copy */
  ATFIX_RESOURCE_INFO srcInfo = { };
  getResourceInfo(pSrcResource, &srcInfo);

  D3D11_BOX srcBox = getResourceBox(&srcInfo, SrcSubresource);
  D3D11_BOX dstBox = getResourceBox(&dstInfo, DstSubresource);

  if (pSrcBox)
    srcBox = *pSrcBox;

  uint32_t w = std::min(srcBox.right - srcBox.left, dstBox.right - DstX);
  uint32_t h = std::min(srcBox.bottom - srcBox.top, dstBox.bottom - DstY);
  uint32_t d = std::min(srcBox.back - srcBox.front, dstBox.back - DstZ);

  srcBox = { srcBox.left,     srcBox.top,     srcBox.front,
             srcBox.left + w, srcBox.top + h, srcBox.front + d };

  dstBox = { DstX,     DstY,     DstZ,
             DstX + w, DstY + h, DstZ + d };

  if (!w || !h || !d)
    return S_OK;

  /* Check if we can map the destination resource immediately. The
   * engine creates all buffers that cause the severe stalls right
   * before mapping them, so this should succeed. */
  D3D11_MAPPED_SUBRESOURCE dstSr;
  D3D11_MAPPED_SUBRESOURCE srcSr;
  HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;

  if (dstInfo.Usage == D3D11_USAGE_DYNAMIC) {
    /* Don't bother with dynamic images etc., haven't seen a situation where it's relevant */
    if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER && w == dstInfo.Width)
      hr = pContext->Map(pDstResource, DstSubresource, D3D11_MAP_WRITE_DISCARD, 0, &dstSr);
  } else {
    hr = pContext->Map(pDstResource, DstSubresource, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &dstSr);
  }

  if (FAILED(hr)) {
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
      log("Failed to map destination resource, hr 0x", std::hex, hr);
      log("Resource dim ", dstInfo.Dim, ", size ", dstInfo.Width , "x", dstInfo.Height, ", usage ", dstInfo.Usage);
    }
    return hr;
  }

  ID3D11Resource* shadowResource = nullptr;

  if (!isCpuReadableResource(&srcInfo)) {
    shadowResource = getOrCreateShadowResource(pContext, pSrcResource);
    hr = pContext->Map(shadowResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      shadowResource->Release();

      log("Failed to map shadow resource, hr 0x", std::hex, hr);
      pContext->Unmap(pDstResource, DstSubresource);
      return hr;
    }
  } else {
    hr = pContext->Map(pSrcResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      log("Failed to map source resource, hr 0x", std::hex, hr);
      log("Resource dim ", srcInfo.Dim, ", size ", srcInfo.Width , "x", srcInfo.Height, ", usage ", srcInfo.Usage);
      pContext->Unmap(pDstResource, DstSubresource);
      return hr;
    }
  }

  /* Do the copy */
  if (dstInfo.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    std::memcpy(
      ptroffset(dstSr.pData, dstBox.left),
      ptroffset(srcSr.pData, srcBox.left), w);
  } else {
    uint32_t formatSize = getFormatPixelSize(dstInfo.Format);

    for (uint32_t z = 0; z < d; z++) {
      for (uint32_t y = 0; y < h; y++) {
        uint32_t dstOffset = (dstBox.left) * formatSize
                           + (dstBox.top + y) * dstSr.RowPitch
                           + (dstBox.front + z) * dstSr.DepthPitch;
        uint32_t srcOffset = (srcBox.left) * formatSize
                           + (srcBox.top + y) * srcSr.RowPitch
                           + (srcBox.front + z) * srcSr.DepthPitch;
        std::memcpy(
          ptroffset(dstSr.pData, dstOffset),
          ptroffset(srcSr.pData, srcOffset),
          w * formatSize);
      }
    }
  }

  pContext->Unmap(pDstResource, DstSubresource);

  if (shadowResource) {
    pContext->Unmap(shadowResource, SrcSubresource);
    shadowResource->Release();
  } else {
    pContext->Unmap(pSrcResource, SrcSubresource);
  }

  return S_OK;
}

class DeviceWrapper final : public ID3D11Device, IDXGIDevice1 {
  LONG refcnt;
  ID3D11Device* dev;
  IDXGIDevice1* dxgi;
  UINT rtWidth = 0;
  UINT rtHeight = 0;

public:
  DeviceWrapper(ID3D11Device* dev_) : refcnt(1), dev(dev_), dxgi(nullptr) {
    dev->QueryInterface(IID_PPV_ARGS(&dxgi));
  }

  // IUnknown
  HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
    if (IsEqualIID(riid, IID_DeviceWrapper)) {
      AddRef();
      *ppvObject = this;
      return S_OK;
    }
    if (IsEqualIID(riid, __uuidof(ID3D11Device))) {
      AddRef();
      *ppvObject = static_cast<ID3D11Device*>(this);
      return S_OK;
    }
    if (dxgi && (
        IsEqualGUID(riid, __uuidof(IDXGIDevice1)) ||
        IsEqualGUID(riid, __uuidof(IDXGIDevice)) ||
        IsEqualGUID(riid, __uuidof(IDXGIObject)))) {
      AddRef();
      *ppvObject = static_cast<IDXGIDevice1*>(this);
      return S_OK;
    }
    HRESULT res = dev->QueryInterface(riid, ppvObject);
    static const GUID IID_ID3D12Device = { 0x189819F1, 0x1DB6, 0x4B57, {0xBE, 0x54, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7} };
    if (IsEqualGUID(riid, IID_ID3D12Device))
      return res;

    LPOLESTR iidstr;
    if (StringFromIID(riid, &iidstr) == S_OK) {
      char buf[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, iidstr, -1, buf, sizeof(buf), nullptr, nullptr);
      log("ID3D11Device QueryInterface ", buf, " => ", std::hex, res);
      CoTaskMemFree(iidstr);
    }
    else {
      log("ID3D11Device QueryInterface <failed to get iid str>");
    }
    return res;
  }

  ID3D11Device* Dev() { return dev; }

  ULONG AddRef() override { return InterlockedAdd(&refcnt, 1); }
  ULONG Release() override {
    ULONG res = InterlockedAdd(&refcnt, -1);
    if (res == 0) {
      dev->Release();
      if (dxgi)
          dxgi->Release();
      delete this;
    }
    return res;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override { return dxgi->GetParent(riid, ppParent); }
  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) override { return dxgi->GetAdapter(pAdapter); }
  HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC* pDesc, UINT NumSurfaces, DXGI_USAGE Usage, const DXGI_SHARED_RESOURCE* pSharedResource, IDXGISurface** ppSurface) override { return dxgi->CreateSurface(pDesc, NumSurfaces, Usage, pSharedResource, ppSurface); }
  HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const* ppResources, DXGI_RESIDENCY* pResidencyStatus, UINT NumResources) override { return dxgi->QueryResourceResidency(ppResources, pResidencyStatus, NumResources); }
  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override { return dxgi->SetGPUThreadPriority(Priority); }
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override { return dxgi->GetGPUThreadPriority(pPriority); }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { return dxgi->SetMaximumFrameLatency(MaxLatency); }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override { return dxgi->GetMaximumFrameLatency(pMaxLatency); }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) override { return dev->CreateUnorderedAccessView(pResource, pDesc, ppUAView); }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override { return dev->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView); }
  HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override { return dev->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) override { return dev->CreateVertexShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader); }
  HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override { return dev->CreateGeometryShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader); }
  HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override { return dev->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader); }
  HRESULT STDMETHODCALLTYPE CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader) override { return dev->CreateHullShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader); }
  HRESULT STDMETHODCALLTYPE CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader) override { return dev->CreateDomainShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader); }
  HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) override { return dev->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader); }
  HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage** ppLinkage) override { return dev->CreateClassLinkage(ppLinkage); }
  HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) override { return dev->CreateBlendState(pBlendStateDesc, ppBlendState); }
  HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D11DepthStencilState** ppDepthStencilState) override { return dev->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState); }
  HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) override { return dev->CreateQuery(pQueryDesc, ppQuery); }
  HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc, ID3D11Predicate** ppPredicate) override { return dev->CreatePredicate(pPredicateDesc, ppPredicate); }
  HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc, ID3D11Counter** ppCounter) override { return dev->CreateCounter(pCounterDesc, ppCounter); }
  HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override { return dev->OpenSharedResource(hResource, ReturnedInterface, ppResource); }
  HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override { return dev->CheckFormatSupport(Format, pFormatSupport); }
  HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) override { return dev->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }
  void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) override { dev->CheckCounterInfo(pCounterInfo); }
  HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType, UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override { return dev->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }
  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override { return dev->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override { return dev->GetPrivateData(guid, pDataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override { return dev->SetPrivateData(guid, DataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override { return dev->SetPrivateDataInterface(guid, pData); }
  D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) override { return dev->GetFeatureLevel(); }
  UINT STDMETHODCALLTYPE GetCreationFlags(void) override { return dev->GetCreationFlags(); }
  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason(void) override { return dev->GetDeviceRemovedReason(); }
  HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override { return dev->SetExceptionMode(RaiseFlags); }
  UINT STDMETHODCALLTYPE GetExceptionMode(void) override { return dev->GetExceptionMode(); }

  HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) override {
    D3D11_SAMPLER_DESC desc = *pSamplerDesc;
    if (config.anisotropy && desc.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
      desc.Filter = D3D11_FILTER_ANISOTROPIC;
      desc.MaxAnisotropy = config.anisotropy;
    }
    return dev->CreateSamplerState(&desc, ppSamplerState);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) override {
    ID3D11Texture2D* tex = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    if (SUCCEEDED(pResource->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC tdesc;
      tex->GetDesc(&tdesc);
      if (tdesc.Format == DXGI_FORMAT_R16_UNORM && pDesc->Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // Our modified shadow texture
        desc = *pDesc;
        desc.Format = DXGI_FORMAT_R16_UNORM;
        pDesc = &desc;
      }
      tex->Release();
    }
    return dev->CreateShaderResourceView(pResource, pDesc, ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override {
    ID3D11Texture2D* tex = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC desc;
    if (SUCCEEDED(pResource->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC tdesc;
      tex->GetDesc(&tdesc);
      if (tdesc.Format == DXGI_FORMAT_R16_UNORM && pDesc->Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // Our modified shadow texture
        desc = *pDesc;
        desc.Format = DXGI_FORMAT_R16_UNORM;
        pDesc = &desc;
      }
      if (config.msaaSamples && (tdesc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || tdesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) && !(isPowerOfTwo(tdesc.Width) && !isPowerOfTwo(tdesc.Height))) {
        // Not shadow texture
        MSAACandidateState state = MSAACandidateState::None;
        UINT size = sizeof(state);
        if (FAILED(pResource->GetPrivateData(IID_MSAACandidate, &size, &state)) || state == MSAACandidateState::None)
          pResource->SetPrivateData(IID_MSAACandidate, sizeof(state), &MSAACandidateStatePossible);
      }
      tex->Release();
    }
    return dev->CreateRenderTargetView(pResource, pDesc, ppRTView);
  }

  HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) override {
    const void* bytecode = pShaderBytecode;
    SIZE_T length = BytecodeLength;
    void* converted = nullptr;
    Buffer buffer;
    if ((buffer = getReplacementShader(pShaderBytecode, BytecodeLength)).data) {
      bytecode = buffer.data;
      length = buffer.length;
    }
    if (config.msaaSamples > 1 && shouldUseSampleRate(pShaderBytecode, BytecodeLength)) {
      converted = convertShaderToSampleRate(bytecode, length);
    } else if (config.msaaSamples > 1 && config.sampleRateAlpha && (buffer = getAlphaToCoverageShader(pShaderBytecode, BytecodeLength)).data) {
      bytecode = buffer.data;
      length = buffer.length;
    }
    HRESULT res = dev->CreatePixelShader(converted ? converted : bytecode, length, pClassLinkage, ppPixelShader);
    if (config.msaaSamples > 1 && !config.sampleRateAlpha && !converted && SUCCEEDED(res) && (buffer = getAlphaToCoverageShader(pShaderBytecode, BytecodeLength)).data) {
      ID3D11PixelShader* a2cShader;
      if (SUCCEEDED(dev->CreatePixelShader(buffer.data, buffer.length, pClassLinkage, &a2cShader))) {
        (*ppPixelShader)->SetPrivateDataInterface(IID_AlphaToCoverage, a2cShader);
        a2cShader->Release();
      }
    }
    if (SUCCEEDED(res) && config.allowShaderToggle && (converted || pShaderBytecode != bytecode)) {
      ID3D11PixelShader* original;
      if (SUCCEEDED(dev->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, &original))) {
        (*ppPixelShader)->SetPrivateDataInterface(IID_OriginalShader, original);
        original->Release();
      }
    }
    if (converted)
      free(converted);
    return res;
  }

  HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, ID3D11RasterizerState** ppRasterizerState) override {
    D3D11_RASTERIZER_DESC desc = *pRasterizerDesc;
    if (config.msaaSamples > 1)
      desc.MultisampleEnable = TRUE;
    return dev->CreateRasterizerState(&desc, ppRasterizerState);
  }

  HRESULT STDMETHODCALLTYPE CreateBuffer(
    const D3D11_BUFFER_DESC*        pDesc,
    const D3D11_SUBRESOURCE_DATA*   pData,
          ID3D11Buffer**            ppBuffer) override {
    D3D11_BUFFER_DESC desc;

    if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
      desc = *pDesc;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      pDesc = &desc;
    }

    return dev->CreateBuffer(pDesc, pData, ppBuffer);
  }

  void STDMETHODCALLTYPE GetImmediateContext(
          ID3D11DeviceContext**     ppImmediateContext) override {
    dev->GetImmediateContext(ppImmediateContext);
    *ppImmediateContext = hookContext(*ppImmediateContext);
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext(
          UINT                      Flags,
          ID3D11DeviceContext**     ppDeferredContext) override {
    HRESULT hr = dev->CreateDeferredContext(Flags, ppDeferredContext);

    if (SUCCEEDED(hr) && ppDeferredContext)
      *ppDeferredContext = hookContext(*ppDeferredContext);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateTexture1D(
    const D3D11_TEXTURE1D_DESC*     pDesc,
    const D3D11_SUBRESOURCE_DATA*   pData,
          ID3D11Texture1D**         ppTexture) override {
    D3D11_TEXTURE1D_DESC desc;

    if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
      desc = *pDesc;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      pDesc = &desc;
    }

    return dev->CreateTexture1D(pDesc, pData, ppTexture);
  }

  HRESULT STDMETHODCALLTYPE CreateTexture2D(
    const D3D11_TEXTURE2D_DESC*     pDesc,
    const D3D11_SUBRESOURCE_DATA*   pData,
          ID3D11Texture2D**         ppTexture) override {
    D3D11_TEXTURE2D_DESC desc;

    if (pDesc)
    {
      if (pDesc->Usage == D3D11_USAGE_STAGING) {
        desc = *pDesc;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        pDesc = &desc;
      }

      if (isPowerOfTwo(pDesc->Width) && isPowerOfTwo(pDesc->Height) && pDesc->Format == DXGI_FORMAT_B8G8R8A8_TYPELESS && (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET)) {
        // Unless someone has a really weird display resolution, this is the shadow texture
        // Increase its bit depth to prevent glitchy shadows
        desc = *pDesc;
        desc.Format = DXGI_FORMAT_R16_UNORM;
        pDesc = &desc;
      }
      else if (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL && rtWidth == 0)
      {
        // Game always creates the main render target first
        // Take advantage of that to figure out its dimensions
        rtWidth = pDesc->Width;
        rtHeight = pDesc->Height;
        log("Guessed main RT size: ", std::dec, rtWidth, "x", rtHeight);
      }
      else if (rtWidth && pDesc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) && pDesc->Width == 1920 && pDesc->Height == 1080)
      {
        // Some older Atelier games (Rorona, Meruru, Ayesha) always render at 1080p no matter the requested resolution
        // Resize that to the full RT size
        log("Resizing 1920x1080 texture to ", std::dec, rtWidth, "x", rtHeight);
        desc = *pDesc;
        desc.Width = rtWidth;
        desc.Height = rtHeight;
        pDesc = &desc;
      }
    }

    return dev->CreateTexture2D(pDesc, pData, ppTexture);
  }

  HRESULT STDMETHODCALLTYPE CreateTexture3D(
    const D3D11_TEXTURE3D_DESC*     pDesc,
    const D3D11_SUBRESOURCE_DATA*   pData,
          ID3D11Texture3D**         ppTexture) override {
    D3D11_TEXTURE3D_DESC desc;

    if (pDesc && pDesc->Usage == D3D11_USAGE_STAGING) {
      desc = *pDesc;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
      pDesc = &desc;
    }

    return dev->CreateTexture3D(pDesc, pData, ppTexture);
  }
};

class ContextWrapper final : public ID3D11DeviceContext {
  LONG refcnt;
  UINT numIndexedDraws = 0;
  ID3D11DeviceContext* ctx;
  ID3D11Resource* needsResolve;
  ID3D11BlendState* alphaToCoverageBlend = nullptr;
  ID3D11BlendState* requestedBlend = nullptr;
  ID3D11PixelShader* requestedPS = nullptr;
  D3D11_MAPPED_SUBRESOURCE lastMap;
  UINT scissorWidth, scissorHeight, viewportWidth, viewportHeight;
  bool blendStateSupportsA2C = false;
  bool psSupportsA2C = false;
  bool rtsizeDirty = false;

public:
  ContextWrapper(ID3D11DeviceContext* ctx_) : refcnt(1), ctx(ctx_) {}

  // IUnknown
  HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
    LPOLESTR iidstr;
    if (StringFromIID(riid, &iidstr) == S_OK) {
      char buf[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, iidstr, -1, buf, sizeof(buf), nullptr, nullptr);
      log("ID3D11DeviceContext QueryInterface ", buf);
      CoTaskMemFree(iidstr);
    } else {
      log("ID3D11DeviceContext QueryInterface <failed to get iid str>");
    }
    return ctx->QueryInterface(riid, ppvObject);
  }
  ULONG AddRef() override { return InterlockedAdd(&refcnt, 1); }
  ULONG Release() override {
    ULONG res = InterlockedAdd(&refcnt, -1);
    if (res == 0) {
      if (alphaToCoverageBlend)
        alphaToCoverageBlend->Release();
      ctx->Release();
      delete this;
    }
    return res;
  }

  bool ShouldUseA2C() const { return psSupportsA2C && blendStateSupportsA2C; }

  ID3D11BlendState* GetAlphaToCoverageBlend() {
    if (!alphaToCoverageBlend) {
      ID3D11Device* dev;
      ctx->GetDevice(&dev);
      D3D11_BLEND_DESC desc = {};
      desc.AlphaToCoverageEnable = TRUE;
      desc.IndependentBlendEnable = TRUE;
      for (D3D11_RENDER_TARGET_BLEND_DESC& target : desc.RenderTarget)
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      dev->CreateBlendState(&desc, &alphaToCoverageBlend);
      dev->Release();
    }
    return alphaToCoverageBlend;
  }

  // ID3D11DeviceChild
  void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override { ctx->GetDevice(ppDevice); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override { return ctx->GetPrivateData(guid, pDataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override { return ctx->SetPrivateData(guid, DataSize, pData); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override { return ctx->SetPrivateDataInterface(guid, pData); }

  // ID3D11DeviceContext
  void VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->PSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances); }
  void PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void IASetInputLayout(ID3D11InputLayout* pInputLayout) override { ctx->IASetInputLayout(pInputLayout); }
  void IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets) override { ctx->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
  void IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override { ctx->IASetIndexBuffer(pIndexBuffer, Format, Offset); }
  void GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->GSSetShader(pShader, ppClassInstances, NumClassInstances); }
  void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override { ctx->IASetPrimitiveTopology(Topology); }
  void VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->VSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void Begin(ID3D11Asynchronous* pAsync) override { ctx->Begin(pAsync); }
  void End(ID3D11Asynchronous* pAsync) override { ctx->End(pAsync); }
  HRESULT GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) override { return ctx->GetData(pAsync, pData, DataSize, GetDataFlags); }
  void SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) override { ctx->SetPredication(pPredicate, PredicateValue); }
  void GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->GSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) override { ctx->OMSetDepthStencilState(pDepthStencilState, StencilRef); }
  void SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets, const UINT* pOffsets) override { ctx->SOSetTargets(NumBuffers, ppSOTargets, pOffsets); }
  void RSSetState(ID3D11RasterizerState* pRasterizerState) override { ctx->RSSetState(pRasterizerState); }
  void CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView* pSrcView) override { ctx->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView); }
  void GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) override { ctx->GenerateMips(pShaderResourceView); }
  void SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) override { ctx->SetResourceMinLOD(pResource, MinLOD); }
  FLOAT GetResourceMinLOD(ID3D11Resource* pResource) override { return ctx->GetResourceMinLOD(pResource); }
  void ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override { ctx->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
  void ExecuteCommandList(ID3D11CommandList* pCommandList, BOOL RestoreContextState) override { ctx->ExecuteCommandList(pCommandList, RestoreContextState); }
  void HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->HSSetShader(pHullShader, ppClassInstances, NumClassInstances); }
  void HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->HSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances); }
  void DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->DSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override { ctx->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts) override { ctx->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts); }
  void CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override { ctx->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances); }
  void CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) override { ctx->CSSetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) override { ctx->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void PSGetShader(ID3D11PixelShader** ppPixelShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances); }
  void PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->PSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void VSGetShader(ID3D11VertexShader** ppVertexShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances); }
  void PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void IAGetInputLayout(ID3D11InputLayout** ppInputLayout) override { ctx->IAGetInputLayout(ppInputLayout); }
  void IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppVertexBuffers, UINT* pStrides, UINT* pOffsets) override { ctx->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets); }
  void IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format, UINT* Offset) override { ctx->IAGetIndexBuffer(pIndexBuffer, Format, Offset); }
  void GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void GSGetShader(ID3D11GeometryShader** ppGeometryShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances); }
  void IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) override { ctx->IAGetPrimitiveTopology(pTopology); }
  void VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->VSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) override { ctx->GetPredication(ppPredicate, pPredicateValue); }
  void GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->GSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView) override { ctx->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView); }
  void OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override { ctx->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews); }
  void OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4], UINT* pSampleMask) override { ctx->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask); }
  void OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState, UINT* pStencilRef) override { ctx->OMGetDepthStencilState(ppDepthStencilState, pStencilRef); }
  void SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) override { ctx->SOGetTargets(NumBuffers, ppSOTargets); }
  void RSGetState(ID3D11RasterizerState** ppRasterizerState) override { ctx->RSGetState(ppRasterizerState); }
  void RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) override { ctx->RSGetViewports(pNumViewports, pViewports); }
  void RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) override { ctx->RSGetScissorRects(pNumRects, pRects); }
  void HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void HSGetShader(ID3D11HullShader** ppHullShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances); }
  void HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->HSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void DSGetShader(ID3D11DomainShader** ppDomainShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances); }
  void DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->DSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView** ppShaderResourceViews) override { ctx->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews); }
  void CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) override { ctx->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews); }
  void CSGetShader(ID3D11ComputeShader** ppComputeShader, ID3D11ClassInstance** ppClassInstances, UINT* pNumClassInstances) override { ctx->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances); }
  void CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState** ppSamplers) override { ctx->CSGetSamplers(StartSlot, NumSamplers, ppSamplers); }
  void CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers) override { ctx->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers); }
  void ClearState() override { ctx->ClearState(); }
  void Flush() override { ctx->Flush(); }
  D3D11_DEVICE_CONTEXT_TYPE GetType() override { return ctx->GetType(); }
  UINT GetContextFlags() override { return ctx->GetContextFlags(); }

  void UpdateRenderTargetMSAAStatus() {
    // Sophie seems to only use indexed draws on the main RT, shadow texture, and a texture where they pre-blend ground tiles
    // There's usually only 10-20 ground tile draws, so target 64 as a safe number to avoid the ground tile draws
    if (config.msaaSamples > 1 && numIndexedDraws > 64) {
      ID3D11DepthStencilView* dsv = nullptr;
      ID3D11RenderTargetView* rtv = nullptr;
      MSAACandidateState state = MSAACandidateState::None;
      UINT size = sizeof(state);
      ctx->OMGetRenderTargets(1, &rtv, &dsv);
      if (rtv && dsv) {
        ID3D11Resource* rtvtex = nullptr;
        rtv->GetResource(&rtvtex);
        if (SUCCEEDED(rtvtex->GetPrivateData(IID_MSAACandidate, &size, &state)) && state == MSAACandidateState::Possible) {
          rtvtex->SetPrivateData(IID_MSAACandidate, sizeof(state), &MSAACandidateStateProbable);
          log("Marking texture with ", std::dec, numIndexedDraws, " indexed draws as MSAA target");
        }
        rtvtex->Release();
      }

      if (dsv) dsv->Release();
      if (rtv) rtv->Release();
    }
    numIndexedDraws = 0;
  }

  HRESULT FinishCommandList(BOOL RestoreDeferredContextState, ID3D11CommandList** ppCommandList) override {
    if (needsResolve) {
      resolveIfMSAA(ctx, needsResolve);
      needsResolve = nullptr;
    }
    UpdateRenderTargetMSAAStatus();
    return ctx->FinishCommandList(RestoreDeferredContextState, ppCommandList);
  }

  void RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) override
  {
    rtsizeDirty = true;
    if (NumViewports) {
        viewportWidth = static_cast<UINT>(pViewports->Width);
        viewportHeight = static_cast<UINT>(pViewports->Height);
    }
    ctx->RSSetViewports(NumViewports, pViewports);
  }

  void RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) override
  {
    rtsizeDirty = true;
    if (NumRects) {
        scissorWidth = pRects->right;
        scissorHeight = pRects->bottom;
    }
    ctx->RSSetScissorRects(NumRects, pRects);
  }

  HRESULT Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) override {
    if (ID3D11Resource* shadow = getShadowResource(pResource)) {
      HRESULT res = ctx->Map(shadow, Subresource, D3D11_MAP_READ_WRITE, 0, pMappedResource);
      if (SUCCEEDED(res))
        lastMap = *pMappedResource;
      shadow->Release();
      return res;
    }
    return ctx->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
  }

  void Unmap(ID3D11Resource* pResource, UINT Subresource) override {
    if (ID3D11Resource* shadow = getShadowResource(pResource)) {
      D3D11_MAPPED_SUBRESOURCE map;
      HRESULT res = ctx->Map(pResource, Subresource, D3D11_MAP_WRITE_DISCARD, 0, &map);
      if (SUCCEEDED(res)) {
        ATFIX_RESOURCE_INFO info = {};
        getResourceInfo(pResource, &info);
        BYTE* dst = static_cast<BYTE*>(map.pData);
        const BYTE* src = static_cast<const BYTE*>(lastMap.pData);
        uint32_t dstPitch = map.RowPitch;
        uint32_t srcPitch = lastMap.RowPitch;
        uint32_t pxWide = info.Width * getFormatPixelSize(info.Format);
        for (uint32_t y = 0; y < info.Height; y++) {
          std::memcpy(dst, src, pxWide);
          dst += dstPitch;
          src += srcPitch;
        }
      }
      ctx->Unmap(shadow, Subresource);
      shadow->Release();
    }
    ctx->Unmap(pResource, Subresource);
  }

  void UpdateViewportScissor() {
    rtsizeDirty = false;
    UINT nviewports = 1;
    UINT nscissor = 1;
    D3D11_VIEWPORT vp;
    D3D11_RECT scissor;
    ctx->RSGetViewports(&nviewports, &vp);
    ctx->RSGetScissorRects(&nscissor, &scissor);
    if (nviewports != 1 || nscissor != 1)
        return;
    bool shouldOverride = scissor.top == 0 && scissor.left == 0 && scissorWidth == viewportWidth && scissorHeight == viewportHeight && scissorWidth == 1920 && scissorHeight == 1080;
    vp.Width = static_cast<FLOAT>(viewportWidth);
    vp.Height = static_cast<FLOAT>(viewportHeight);
    scissor.right = scissorWidth;
    scissor.bottom = scissorHeight;
    if (shouldOverride) {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ID3D11Resource* rsrc = nullptr;
        ID3D11Texture2D* tex;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        if (rtv) {
          rtv->GetResource(&rsrc);
          rtv->Release();
        }
        if (dsv) {
          if (!rsrc)
            dsv->GetResource(&rsrc);
          dsv->Release();
        }
        if (rsrc) {
          if (SUCCEEDED(rsrc->QueryInterface(IID_PPV_ARGS(&tex)))) {
            D3D11_TEXTURE2D_DESC desc;
            tex->GetDesc(&desc);
            vp.Width = static_cast<FLOAT>(desc.Width);
            vp.Height = static_cast<FLOAT>(desc.Height);
            scissor.right = desc.Width;
            scissor.bottom = desc.Height;
            tex->Release();
          }
          rsrc->Release();
        }
    }
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &scissor);
  }

  void Draw(UINT VertexCount, UINT StartVertexLocation) override {
     if (rtsizeDirty) UpdateViewportScissor();
     ctx->Draw(VertexCount, StartVertexLocation);
  }
  void DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override {
    numIndexedDraws++;
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
  }
  void DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override {
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
  }
  void DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override {
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
  }
  void DrawAuto() override {
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawAuto();
  }
  void DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override {
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
  }
  void DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs, UINT AlignedByteOffsetForArgs) override {
    if (rtsizeDirty) UpdateViewportScissor();
    ctx->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
  }

  void PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) override {
    for (UINT i = 0; i < NumViews; i++) {
      if (ppShaderResourceViews[i]) {
        ID3D11Resource* res = nullptr;
        ppShaderResourceViews[i]->GetResource(&res);
        resolveIfMSAA(ctx, res);
        res->Release();
      }
    }
    ctx->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
  }

  void PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) override {
    requestedPS = pPixelShader;
    ID3D11PixelShader* a2c = nullptr;
    bool useOriginalShaders = shouldUseOldShaders();
    bool oldUseA2C = ShouldUseA2C();
    if (pPixelShader && !useOriginalShaders) {
      UINT size = sizeof(a2c);
      pPixelShader->GetPrivateData(IID_AlphaToCoverage, &size, &a2c);
    }
    psSupportsA2C = a2c;
    bool newUseA2C = ShouldUseA2C();
    if (newUseA2C)
        pPixelShader = a2c;
    if (newUseA2C != oldUseA2C) {
        FLOAT factor[4];
        UINT sampleMask;
        ctx->OMGetBlendState(nullptr, factor, &sampleMask);
        ctx->OMSetBlendState(newUseA2C ? GetAlphaToCoverageBlend() : requestedBlend, factor, sampleMask);
    }
    ID3D11PixelShader* original = nullptr;
    if (pPixelShader && useOriginalShaders) {
      UINT size = sizeof(original);
      if (SUCCEEDED(pPixelShader->GetPrivateData(IID_OriginalShader, &size, &original)))
        pPixelShader = original;
    }
    ctx->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
    if (a2c)
      a2c->Release();
    if (original)
      original->Release();
  }

  void OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override {
    requestedBlend = pBlendState;
    bool oldUseA2C = ShouldUseA2C();
    if (pBlendState) {
      D3D11_BLEND_DESC desc;
      pBlendState->GetDesc(&desc);
      bool ok = true;
      for (D3D11_RENDER_TARGET_BLEND_DESC& target : desc.RenderTarget)
        ok &= !target.BlendEnable;
      blendStateSupportsA2C = ok;
    } else {
      blendStateSupportsA2C = false;
    }
    bool newUseA2C = ShouldUseA2C();
    if (!newUseA2C)
      ctx->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
    else if (!oldUseA2C)
      ctx->OMSetBlendState(GetAlphaToCoverageBlend(), BlendFactor, SampleMask);
  }

  void ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override {
    if (ID3D11DepthStencilView* msaa = getMSAADSV(pDepthStencilView)) {
      ctx->ClearDepthStencilView(msaa, ClearFlags, Depth, Stencil);
      msaa->Release();
    } else {
      ctx->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
    }
  }

  void ClearRenderTargetView(
          ID3D11RenderTargetView*   pRTV,
    const FLOAT                     pColor[4]) override {
    if (ID3D11RenderTargetView* msaa = getMSAARTV(pRTV)) {
      ctx->ClearRenderTargetView(msaa, pColor);
      msaa->Release();
    } else {
      ctx->ClearRenderTargetView(pRTV, pColor);
    }
    if (pRTV)
      updateViewShadowResource(ctx, pRTV);
  }

  void ClearUnorderedAccessViewFloat(
          ID3D11UnorderedAccessView* pUAV,
    const FLOAT                     pColor[4]) override {
    ctx->ClearUnorderedAccessViewFloat(pUAV, pColor);
    if (pUAV)
      updateViewShadowResource(ctx, pUAV);
  }

  void ClearUnorderedAccessViewUint(
          ID3D11UnorderedAccessView* pUAV,
    const UINT                      pColor[4]) override {
    ctx->ClearUnorderedAccessViewUint(pUAV, pColor);
    if (pUAV)
      updateViewShadowResource(ctx, pUAV);
  }

  void CopyResource(
          ID3D11Resource*           pDstResource,
          ID3D11Resource*           pSrcResource) override {
    ID3D11Resource* dstShadow = getShadowResource(pDstResource);

    resolveIfMSAA(ctx, pSrcResource);

    bool needsBaseCopy = true;
    bool needsShadowCopy = true;

    if (isImmediatecontext(ctx)) {
      HRESULT hr = tryCpuCopy(ctx, pDstResource,
        0, 0, 0, 0, pSrcResource, 0, nullptr);
      needsBaseCopy = FAILED(hr);

      if (!needsBaseCopy && dstShadow) {
        hr = tryCpuCopy(ctx, dstShadow,
          0, 0, 0, 0, pSrcResource, 0, nullptr);
        needsShadowCopy = FAILED(hr);
      }
    }

    if (needsBaseCopy)
      ctx->CopyResource(pDstResource, pSrcResource);

    if (dstShadow) {
      if (needsShadowCopy)
        ctx->CopyResource(dstShadow, pSrcResource);

      dstShadow->Release();
    }
  }

  void CopySubresourceRegion(
          ID3D11Resource*           pDstResource,
          UINT                      DstSubresource,
          UINT                      DstX,
          UINT                      DstY,
          UINT                      DstZ,
          ID3D11Resource*           pSrcResource,
          UINT                      SrcSubresource,
    const D3D11_BOX*                pSrcBox) override {

    ID3D11Resource* dstShadow = getShadowResource(pDstResource);

    resolveIfMSAA(ctx, pSrcResource);

    bool needsBaseCopy = true;
    bool needsShadowCopy = true;

    if (isImmediatecontext(ctx)) {
      HRESULT hr = tryCpuCopy(ctx,
        pDstResource, DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
      needsBaseCopy = FAILED(hr);

      if (!needsBaseCopy && dstShadow) {
        hr = tryCpuCopy(ctx,
          dstShadow,    DstSubresource, DstX, DstY, DstZ,
          pSrcResource, SrcSubresource, pSrcBox);
        needsShadowCopy = FAILED(hr);
      }
    }

    if (needsBaseCopy) {
      ctx->CopySubresourceRegion(
        pDstResource, DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
    }

    if (dstShadow) {
      if (needsShadowCopy) {
        ATFIX_RESOURCE_INFO srcInfo = { };
        getResourceInfo(pSrcResource, &srcInfo);

        ctx->CopySubresourceRegion(
          dstShadow,    DstSubresource, DstX, DstY, DstZ,
          pSrcResource, SrcSubresource, pSrcBox);
      }

      dstShadow->Release();
    }
  }

  void Dispatch(
          UINT                      X,
          UINT                      Y,
          UINT                      Z) override {
    ctx->Dispatch(X, Y, Z);
    updateUavShadowResources(ctx);
  }

  void DispatchIndirect(
          ID3D11Buffer*             pParameterBuffer,
          UINT                      pParameterOffset) override {
    ctx->DispatchIndirect(pParameterBuffer, pParameterOffset);
    updateUavShadowResources(ctx);
  }

  void OMSetRenderTargets(
          UINT                      RTVCount,
          ID3D11RenderTargetView* const* ppRTVs,
          ID3D11DepthStencilView*   pDSV) override {
    updateRtvShadowResources(ctx);
    rtsizeDirty = true;

    UpdateRenderTargetMSAAStatus();

    ID3D11RenderTargetView* msaaTex = nullptr;
    ID3D11DepthStencilView* msaaDepth = nullptr;

    if (config.msaaSamples > 1 && ppRTVs && RTVCount == 1 && pDSV) {
      ID3D11Resource* base = nullptr;
      ppRTVs[0]->GetResource(&base);
      MSAACandidateState state = MSAACandidateState::None;
      UINT size = sizeof(state);
      if (SUCCEEDED(base->GetPrivateData(IID_MSAACandidate, &size, &state)) && state >= MSAACandidateState::Probable) {
        ID3D11Device* dev;
        ctx->GetDevice(&dev);
        msaaTex = getOrCreateMSAARTV(dev, ppRTVs[0]);
        msaaDepth = getOrCreateMSAADSV(dev, pDSV);
        ppRTVs = &msaaTex;
        pDSV = msaaDepth;
        dev->Release();
        // We're rendering to the texture so it's now dirty
        base->SetPrivateData(IID_MSAACandidate, sizeof(state), &MSAACandidateStateDirty);
        needsResolve = base;
      }
      base->Release();
    }

    ctx->OMSetRenderTargets(RTVCount, ppRTVs, pDSV);

    if (msaaTex) msaaTex->Release();
    if (msaaDepth) msaaDepth->Release();
  }

  void OMSetRenderTargetsAndUnorderedAccessViews(
          UINT                      RTVCount,
          ID3D11RenderTargetView* const* ppRTVs,
          ID3D11DepthStencilView*   pDSV,
          UINT                      UAVIndex,
          UINT                      UAVCount,
          ID3D11UnorderedAccessView* const* ppUAVs,
    const UINT*                     pUAVClearValues) override {
    updateRtvShadowResources(ctx);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(
      RTVCount, ppRTVs, pDSV, UAVIndex, UAVCount, ppUAVs, pUAVClearValues);
  }

  void UpdateSubresource(
          ID3D11Resource*           pResource,
          UINT                      Subresource,
    const D3D11_BOX*                pBox,
    const void*                     pData,
          UINT                      RowPitch,
          UINT                      SlicePitch) override {
    ctx->UpdateSubresource(
      pResource, Subresource, pBox, pData, RowPitch, SlicePitch);

    ID3D11Resource* shadowResource = getShadowResource(pResource);
    if (shadowResource) {
      ctx->UpdateSubresource(
        shadowResource, Subresource, pBox, pData, RowPitch, SlicePitch);
      shadowResource->Release();
    }
  }
};

typedef HRESULT (STDMETHODCALLTYPE*PFN_IDXGIFactory_CreateSwapChain)(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain);

namespace originals {
  static PFN_IDXGIFactory_CreateSwapChain IDXGIFactory_CreateSwapChain;
}

namespace replacements {
  static HRESULT STDMETHODCALLTYPE IDXGIFactory_CreateSwapChain(IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** ppSwapChain)
  {
    DeviceWrapper* wrapper;
    if (SUCCEEDED(dev->QueryInterface(IID_DeviceWrapper, reinterpret_cast<void**>(&wrapper)))) {
      dev = wrapper->Dev();
      wrapper->Release();
    }
    return originals::IDXGIFactory_CreateSwapChain(self, dev, desc, ppSwapChain);
  }
}

void hookIDXGIDevice() {
  IDXGIFactory* factory;
  if (SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) {
    void** vtable = *reinterpret_cast<void***>(factory);
    if (vtable[10] != replacements::IDXGIFactory_CreateSwapChain) {
      DWORD prot;
      VirtualProtect(&vtable[10], sizeof(void*), PAGE_READWRITE, &prot);
      originals::IDXGIFactory_CreateSwapChain = reinterpret_cast<PFN_IDXGIFactory_CreateSwapChain>(vtable[10]);
      vtable[10] = replacements::IDXGIFactory_CreateSwapChain;
      VirtualProtect(&vtable[10], sizeof(void*), prot, &prot);
    }
    factory->Release();
  }
}

ID3D11Device* hookDevice(ID3D11Device* pDevice) {
  hookIDXGIDevice();
  log("Hooking device ", pDevice);
  return new DeviceWrapper(pDevice);
}

ID3D11DeviceContext* hookContext(ID3D11DeviceContext* pContext) {
  return new ContextWrapper(pContext);
}

}