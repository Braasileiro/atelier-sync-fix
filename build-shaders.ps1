[void](New-Item -ItemType directory -Force -Name Shaders)

$header  = "#include `"Shaders.h`"`n`n"
$header += "namespace atfix {`n"
Set-Content -NoNewline -Path Shaders.cpp $header

$shaders = [System.Collections.ArrayList]@()
$shadersA2C = @{
	0 = [System.Collections.ArrayList]@();
	2 = [System.Collections.ArrayList]@();
	4 = [System.Collections.ArrayList]@();
	8 = [System.Collections.ArrayList]@();
	16 = [System.Collections.ArrayList]@();
}

function Add-Shader {
	[CmdletBinding()]
	param(
		[Parameter(Mandatory=$true)]
		[string]$HashString,
		[Parameter(Mandatory=$true)]
		[string]$File,
		[switch]$AlphaToCoverage,
		[string[]]$FxcArgs
	)
	$hash = $HashString.Split("-") | % { [System.Convert]::ToUInt32($_, 16) }
	$variants = if ($AlphaToCoverage) { 0, 2, 4, 8, 16 } else { 0 }
	foreach ($variant in $variants) {
		$a2cstr = if ($AlphaToCoverage) { "_a2c$variant" } else { "" }
		$hashDecl = "0x{0:x8}, 0x{1:x8}, 0x{2:x8}, 0x{3:x8}" -f $hash[0], $hash[1], $hash[2], $hash[3]
		$name = "shader${a2cstr}_{0:x8}_{1:x8}_{2:x8}_{3:x8}" -f $hash[0], $hash[1], $hash[2], $hash[3]
		$decl = "{{$hashDecl}, $name, sizeof($name)}"
		$path = "Shaders\$name.fxc"
		$extra = if ($AlphaToCoverage) { "/DMSAA_SAMPLE_COUNT=$variant" } else { "" }

		fxc /T ps_5_0 /Fo $path $FxcArgs $extra $File
		$content = [System.Io.File]::ReadAllBytes($path)
		$array = "{"

		for ($i = 0; $i -lt $content.Length; $i += 4) {
			$num = [UInt32]$content[$i] -bor ([UInt32]$content[$i + 1] -shl 8) -bor ([UInt32]$content[$i + 2] -shl 16) -bor ([UInt32]$content[$i + 3] -shl 24)
			if ($i % 32 -eq 0) {
				$array += "`n  "
			} else {
				$array += " "
			}
			$array += "0x{0:x8}," -f $num
		}

		$array += "`n};"

		Add-Content -NoNewline -Path Shaders.cpp "constexpr static DWORD $name[] = $array`n"

		if ($AlphaToCoverage) {
			$shadersA2C[$variant].Add($decl)
		} else {
			$shaders.Add($decl)
		}
	}
}

Add-Shader -File Shadows.hlsl -Hash "e239813e-6a8e1aa0-35138b5b-24edadc5"
Add-Shader -File Shadows.hlsl -Hash "e22cbf18-d696a9b1-c0170a21-8228bf37" -FxcArgs "/DALPHA=1"
Add-Shader -File Shadows.hlsl -Hash "8873fdd5-ac2a98f0-11c0465a-65bfdcfa" -FxcArgs "/DALPHA=2"

Add-Shader -File Environment.hlsl -Hash "018b76d9-4ea08211-68d529da-7fedc0e7"
Add-Shader -File Environment.hlsl -Hash "a69d13f1-e43a631b-581188da-df4e79d3" -FxcArgs "/DBLEND=1"
Add-Shader -File Environment.hlsl -Hash "1810a761-76dc9271-cc643b04-150364b9" -FxcArgs "/DBLEND=2"
Add-Shader -File Environment.hlsl -Hash "852cf89d-5a4f9d68-85d19c63-2ce60312" -FxcArgs "/DBLEND=3"
Add-Shader -File Environment.hlsl -Hash "b934d303-5527e8e9-2624c06c-83a9ecbc" -FxcArgs "/DBLEND=4"
Add-Shader -File Environment.hlsl -Hash "982709b2-2274a731-43455c36-104eb2a8" -FxcArgs "/DBLEND=5"
Add-Shader -File Environment.hlsl -Hash "d757bed5-09656d6f-43997a9c-c92777f9" -FxcArgs "/DBLEND=6"
Add-Shader -File Environment.hlsl -Hash "0cd1b9e5-22e7069e-476455ff-98bfd850" -FxcArgs "/DALPHA=1"
Add-Shader -File Environment.hlsl -Hash "0cd1b9e5-22e7069e-476455ff-98bfd850" -FxcArgs "/DALPHA=2" -AlphaToCoverage

function Add-ShaderList($name, $array) {
	$line = "constexpr static ShaderReplacement ${name}Data[] = {`n"
	foreach ($item in $array) {
		$line += "  $item,`n"
	}
	$line += "};`n"
	$line += "constexpr ShaderReplacementList ${name}{${Name}Data, _countof(${name}Data)};`n`n"
	if ($array.Count -eq 0) {
		$line = "constexpr ShaderReplacementList ${name}{nullptr, 0};`n"
	}
	Add-Content -NoNewline -Path Shaders.cpp $line
}

Add-Content -NoNewline -Path Shaders.cpp "`n`n"

Add-ShaderList shaderReplacements $shaders
Add-ShaderList shaderReplacementsAlphaToCoverage0 $shadersA2C[0]
Add-ShaderList shaderReplacementsAlphaToCoverage2 $shadersA2C[2]
Add-ShaderList shaderReplacementsAlphaToCoverage4 $shadersA2C[4]
Add-ShaderList shaderReplacementsAlphaToCoverage8 $shadersA2C[8]
Add-ShaderList shaderReplacementsAlphaToCoverage16 $shadersA2C[16]

Add-Content -NoNewline -Path Shaders.cpp "} // namespace atfix`n"