#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName='Create')]
param(
    [Parameter(ParameterSetName='Create')][string]$SourceDirectory,
    [Parameter(ParameterSetName='Create')][string]$Output,
    [Parameter(Mandatory,ParameterSetName='Verify')][string]$Verify,
    [Parameter(Mandatory,ParameterSetName='Directory')][string]$VerifyDirectory
)
# A fixed policy is reused for incoming, installed, rollback and recovery bundles.
& (Join-Path $PSScriptRoot 'package.ps1') @PSBoundParameters -ExperimentalArm64
