#pragma once

#include <d3d11.h>

#include "log.h"

namespace atfix {

ID3D11Device* hookDevice(ID3D11Device* pDevice);
ID3D11DeviceContext* hookContext(ID3D11DeviceContext* pContext);

/* lives in main.cpp */
extern Log log;
extern struct Config {
	DWORD msaaSamples;
	DWORD anisotropy;
	bool ssaaCharacters;
	bool ssaaTransparentObjects;
	bool ssaaAll;
	bool allowShaderToggle;
} config;
}
