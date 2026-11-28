Veyla Wi-Fi stability tools
==========================

Scripturi pentru Windows 10/11. Necesita PowerShell ca Administrator
(scriptul cere elevarea singur).

Ce face wifi-stability-on.ps1
-----------------------------
- opreste power saving-ul adaptorului Wi-Fi, daca driverul expune proprietatea;
- seteaza "Allow the computer to turn off this device" pe oprit (PnPCapabilities);
- seteaza Wireless Adapter Settings din power plan pe Maximum Performance;
- seteaza Roaming Aggressiveness pe cea mai joasa valoare.

NU atinge Wi-Fi 7, MLO, 802.11be, banda sau channel width.

Cum se foloseste
----------------
1. Inchide Veyla (ca sa nu intrerupa streaming-ul cand adaptorul reporneste).
2. Click dreapta pe wifi-stability-on.ps1 -> Run with PowerShell, sau:
     powershell -ExecutionPolicy Bypass -File .\wifi-stability-on.ps1
3. Porneste Veyla si joaca normal.

Preview fara modificari:
     powershell -ExecutionPolicy Bypass -File .\wifi-stability-on.ps1 -DryRun

Fara repornirea adaptorului (necesita reboot pentru PnPCapabilities):
     powershell -ExecutionPolicy Bypass -File .\wifi-stability-on.ps1 -NoRestart

Revert
------
     powershell -ExecutionPolicy Bypass -File .\wifi-stability-off.ps1

Scriptul citeste backup-ul din C:\ProgramData\Veyla\wifi-stability-backup.json
si pune la loc valorile originale. Backup-ul e sters dupa revert.

Daca driverul nu expune nicio proprietate de power save, scriptul scrie asta si
continua cu restul. Pe unele adaptoare MediaTek optiunea se numeste
"Power Saving Mode" si poate aparea doar in driverul actualizat.
