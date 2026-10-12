param([ValidateSet(10000,50000,100000)][int]$RateHz)
$ErrorActionPreference = 'Stop'
$workspacePath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$gnuPath = 'I:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin'
$objectPath = Join-Path $PSScriptRoot "build-objects\adc_validation-${RateHz}.o"
$elfPath = Join-Path $PSScriptRoot "adc-${RateHz}.elf"
$mapPath = Join-Path $PSScriptRoot "adc-${RateHz}.map"
$responsePath = Join-Path $PSScriptRoot "build-objects\objects-${RateHz}.list"
$logPath = Join-Path $PSScriptRoot "build-${RateHz}.log"
New-Item -ItemType Directory -Path (Split-Path $objectPath) -Force | Out-Null
# Use the existing phase-2 objects and compile only the documented diagnostic rate.
# Debug/oscill.elf and its default 10 kSPS objects are not replaced by this script.
$objectList = [IO.File]::ReadAllText((Join-Path $workspacePath 'Debug\objects.list'))
$originalObject = '"./Core/Src/adc_validation.o"'
if (($objectList.Split([string[]]@($originalObject),[StringSplitOptions]::None).Count - 1) -ne 1) {
    throw 'Expected exactly one adc_validation.o in the baseline object list'
}
$objectList = $objectList.Replace($originalObject, '"' + $objectPath.Replace('\','/') + '"')
[IO.File]::WriteAllText($responsePath,$objectList,[Text.Encoding]::ASCII)
$compileArgs = @('../Core/Src/adc_validation.c','-mcpu=cortex-m4','-std=gnu11','-g3',
    '-DDEBUG','-DUSE_HAL_DRIVER','-DSTM32F429xx',"-DADC_CAPTURE_PHYSICAL_RATE_HZ=${RateHz}U",'-c',
    '-I../Core/Inc','-I../Drivers/STM32F4xx_HAL_Driver/Inc',
    '-I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy','-I../Drivers/CMSIS/Device/ST/STM32F4xx/Include',
    '-I../Drivers/CMSIS/Include','-O0','-ffunction-sections','-fdata-sections','-Wall',
    '-fstack-usage','-fcyclomatic-complexity','-MMD','-MP',"-MF$objectPath.d", "-MT$objectPath",
    '--specs=nano.specs','-mfpu=fpv4-sp-d16','-mfloat-abi=hard','-mthumb','-o',$objectPath)
$linkerPath = Join-Path $workspacePath 'STM32F429ZITX_FLASH.ld'
$linkArgs = @('-o',$elfPath,"@$responsePath",'-mcpu=cortex-m4',
    "-T$linkerPath",'--specs=nosys.specs',
    "-Wl,-Map=$mapPath",'-Wl,--gc-sections','-static','--specs=nano.specs',
    '-mfpu=fpv4-sp-d16','-mfloat-abi=hard','-mthumb','-Wl,--start-group','-lc','-lm','-Wl,--end-group')
Push-Location (Join-Path $workspacePath 'Debug')
try {
    "Physical rate: $RateHz Hz; only adc_validation.c rebuilt" | Set-Content -LiteralPath $logPath -Encoding UTF8
    ($compileArgs -join ' ') | Add-Content -LiteralPath $logPath -Encoding UTF8
    & (Join-Path $gnuPath 'arm-none-eabi-gcc.exe') @compileArgs 2>&1 | Tee-Object -FilePath $logPath -Append
    if ($LASTEXITCODE -ne 0) { throw 'Compile failed' }
    & (Join-Path $gnuPath 'arm-none-eabi-gcc.exe') @linkArgs 2>&1 | Tee-Object -FilePath $logPath -Append
    if ($LASTEXITCODE -ne 0) { throw 'Link failed' }
    & (Join-Path $gnuPath 'arm-none-eabi-size.exe') $elfPath | Tee-Object -FilePath $logPath -Append
    $symbols = & (Join-Path $gnuPath 'arm-none-eabi-nm.exe') -S $elfPath
    foreach ($expected in @('20001e40 00001040 B g_adc_dma_storage',
        '20002e80 00000800 B adc_snapshot','20003680 00000ae0 B g_adc_diagnostic')) {
        if ($symbols -notcontains $expected) { throw "Diagnostic memory layout changed: $expected" }
    }
    ($symbols | Select-String 'g_adc_diagnostic|g_adc_dma_storage|adc_snapshot|AC_Checkpoint') |
        Out-String | Add-Content -LiteralPath $logPath -Encoding UTF8
    $elfHash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
    "$elfHash  adc-${RateHz}.elf" | Set-Content -LiteralPath (Join-Path $PSScriptRoot "adc-${RateHz}.sha256") -Encoding ASCII
    Write-Output "BUILT $RateHz Hz: $elfHash"
} finally { Pop-Location }
