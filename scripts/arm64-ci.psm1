#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Get-NskkzimeArm64CiNativeTests {
    @('zenz_prompt','numeric','core','forget','inference_policy','romaji','romaji_trace','registration',
      'dictionary','compiled_dictionary','dictionary_trace','dictionary_missing_file','broker_snapshot',
      'runtime_configuration','tsf_registration','learning','user_dictionary','user_dictionary_cli',
      'dictionary_manager','settings','runtime_dictionary','dictionary_cache','tsf_diagnostics',
      'config_cli','cpu_requirements','input_log','inference')
}
function Get-NskkzimeArm64CiTests {
    Get-NskkzimeArm64CiNativeTests
    foreach($abi in @('arm64','arm64ec','x64','x86')) { 'arm64x_'+$abi+'_tsf_architecture_tests' }
}
function Assert-NskkzimeCiRelativePath([string]$Path) {
    if([string]::IsNullOrEmpty($Path) -or $Path.Contains('\') -or $Path.Contains(':') -or $Path.StartsWith('/') -or
       $Path -match '[\x00-\x1f]' -or @($Path.Split('/') | Where-Object {$_ -in @('','.','..')}).Count){throw 'Invalid CI bundle path.'}
}
Export-ModuleMember -Function Get-NskkzimeArm64CiNativeTests,Get-NskkzimeArm64CiTests,Assert-NskkzimeCiRelativePath
