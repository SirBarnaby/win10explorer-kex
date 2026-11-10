#Requires -RunAsAdministrator
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$windowsInfo=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
if($windowsInfo.CurrentBuild-ne '19045'-or $windowsInfo.EditionID-notin 'Professional','Enterprise','Education'){
 throw 'This reviewed setup is for Windows 10 22H2 Pro, Enterprise or Education, build 19045.'
}
# This is the only system change. Windows decides which prerequisites are needed.
# -NoRestart leaves restart timing to the user. This script never launches the patch.
$result=Enable-WindowsOptionalFeature -Online -FeatureName 'Containers-DisposableClientVM' -All -NoRestart
$result | Select-Object FeatureName,Online,RestartNeeded
Write-Output 'Windows Sandbox was enabled. Save your work and restart when convenient if RestartNeeded is true.'
Write-Output 'No Explorer extension was installed or changed by this script.'
