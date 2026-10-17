#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Status','Plan','Install','Rollback','Unregister','Recover')][string]$Action='Status',
    [string]$PackageDirectory=(Join-Path $PSScriptRoot '..'),
    [string]$Version,
    [string]$ExpectedUserSid
)
& (Join-Path $PSScriptRoot 'install.ps1') @PSBoundParameters -ExperimentalArm64
