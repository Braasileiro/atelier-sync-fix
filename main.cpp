#include <iostream>

#include "impl.h"
#include "util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
  #define DLLEXPORT
#else
  #define DLLEXPORT __declspec(dllexport)
#endif

namespace atfix {

Log log("atfix.log");
Config config { 8, true };

/** Load system D3D11 DLL and return entry points */
using PFN_D3D11CreateDevice = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct D3D11Proc {
  PFN_D3D11CreateDevice             D3D11CreateDevice             = nullptr;
  PFN_D3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain = nullptr;
};

D3D11Proc loadSystemD3D11() {
  static mutex initMutex;
  static D3D11Proc d3d11Proc;

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  std::lock_guard lock(initMutex);

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  HMODULE libD3D11 = LoadLibraryA("d3d11_proxy.dll");

  if (libD3D11) {
    log("Using d3d11_proxy.dll");
  } else {
    std::array<char, MAX_PATH + 1> path = { };

    if (!GetSystemDirectoryA(path.data(), MAX_PATH))
      return D3D11Proc();

    std::strncat(path.data(), "\\d3d11.dll", MAX_PATH);
    log("Using ", path.data());
    libD3D11 = LoadLibraryA(path.data());

    if (!libD3D11) {
      log("Failed to load d3d11.dll (", path.data(), ")");
      return D3D11Proc();
    }
  }

  if (GetFileAttributesA("atfix.ini") == ~0) {
    const char* str = "[MSAA]\n"
                      "; Number of samples (1 = no MSAA)\n"
                      "NumSamples = 8\n"
                      "; Whether to do SSAA on characters (fairly cheap, improves thin lines on clothing)\n"
                      "CharacterSSAA = 1\n"
                      "; Whether to do SSAA on transparent objects like grass and tree leaves (somewhat expensive, but prevents them from shimmering when the camera moves)\n"
                      "ObjectSSAA = 0\n"
                      "; Apply SSAA to everything, because you have more GPU power than you know what to do with\n"
                      "FullSSAA = 0\n"
                      "[Other]\n"
                      "; Allow toggling shader enhancements by holding BACK / SELECT (will not toggle MSAA but will toggle most other things)\n"
                      "EnhancementToggle = 0\n";
    HANDLE file = CreateFileA("atfix.ini", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      WriteFile(file, str, strlen(str), nullptr, nullptr);
      CloseHandle(file);
    }
  } else {
    char NumSamples[8];
    char CharacterSSAA[8];
    char ObjectSSAA[8];
    char FullSSAA[8];
    char UseShaderToggle[8];
    bool ok = true;
    GetPrivateProfileStringA("MSAA", "NumSamples", "8", NumSamples, sizeof(NumSamples), ".\\atfix.ini");
    GetPrivateProfileStringA("MSAA", "CharacterSSAA", "1", CharacterSSAA, sizeof(CharacterSSAA), ".\\atfix.ini");
    GetPrivateProfileStringA("MSAA", "ObjectSSAA", "0", ObjectSSAA, sizeof(ObjectSSAA), ".\\atfix.ini");
    GetPrivateProfileStringA("MSAA", "FullSSAA", "0", FullSSAA, sizeof(FullSSAA), ".\\atfix.ini");
    GetPrivateProfileStringA("Other", "EnhancementToggle", "0", UseShaderToggle, sizeof(UseShaderToggle), ".\\atfix.ini");
    config.msaaSamples = atoi(NumSamples);
    config.ssaaCharacters = atoi(CharacterSSAA);
    config.ssaaTransparentObjects = atoi(ObjectSSAA);
    config.ssaaAll = atoi(FullSSAA);
    config.allowShaderToggle = atoi(UseShaderToggle);
    if (config.msaaSamples < 1)
      config.msaaSamples = 1;
    log("Loaded config, ", config.msaaSamples, " samples, ", config.ssaaCharacters, " characterSSAA, ", config.ssaaTransparentObjects, " objectSSAA, ", config.ssaaAll, " fullSSAA, ", config.allowShaderToggle, " enhancementToggle ");
  }

  d3d11Proc.D3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
    GetProcAddress(libD3D11, "D3D11CreateDevice"));
  d3d11Proc.D3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
    GetProcAddress(libD3D11, "D3D11CreateDeviceAndSwapChain"));

  log("D3D11CreateDevice             @ ", reinterpret_cast<void*>(d3d11Proc.D3D11CreateDevice));
  log("D3D11CreateDeviceAndSwapChain @ ", reinterpret_cast<void*>(d3d11Proc.D3D11CreateDeviceAndSwapChain));
  return d3d11Proc;
}

}

extern "C" {

DLLEXPORT HRESULT __stdcall D3D11CreateDevice(
        IDXGIAdapter*         pAdapter,
        D3D_DRIVER_TYPE       DriverType,
        HMODULE               Software,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        UINT                  SDKVersion,
        ID3D11Device**        ppDevice,
        D3D_FEATURE_LEVEL*    pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext) {
  if (ppDevice)
    *ppDevice = nullptr;

  if (ppImmediateContext)
    *ppImmediateContext = nullptr;

  auto proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDevice)
    return E_FAIL;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  HRESULT hr = (*proc.D3D11CreateDevice)(pAdapter, DriverType, Software,
    Flags, pFeatureLevels, FeatureLevels, SDKVersion, &device, pFeatureLevel,
    &context);

  if (FAILED(hr))
    return hr;

  device = atfix::hookDevice(device);
  context = atfix::hookContext(context);

  if (ppDevice) {
    device->AddRef();
    *ppDevice = device;
  }

  if (ppImmediateContext) {
    context->AddRef();
    *ppImmediateContext = context;
  }

  device->Release();
  context->Release();
  return hr;
}

DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
        IDXGIAdapter*         pAdapter,
        D3D_DRIVER_TYPE       DriverType,
        HMODULE               Software,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        UINT                  SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
        IDXGISwapChain**      ppSwapChain,
        ID3D11Device**        ppDevice,
        D3D_FEATURE_LEVEL*    pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext) {
  if (ppDevice)
    *ppDevice = nullptr;

  if (ppImmediateContext)
    *ppImmediateContext = nullptr;

  if (ppSwapChain)
    *ppSwapChain = nullptr;

  auto proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDeviceAndSwapChain)
    return E_FAIL;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  HRESULT hr = (*proc.D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType, Software,
    Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
    &device, pFeatureLevel, &context);

  if (FAILED(hr))
    return hr;

  device = atfix::hookDevice(device);
  context = atfix::hookContext(context);

  if (ppDevice) {
    device->AddRef();
    *ppDevice = device;
  }

  if (ppImmediateContext) {
    context->AddRef();
    *ppImmediateContext = context;
  }

  device->Release();
  context->Release();
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  return TRUE;
}

}