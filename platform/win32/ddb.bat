@echo off
rem #########################################################################
rem #
rem # DDB initialization script
rem #
rem #########################################################################

set DDBBASE=%~dp0
set PROJ_LIB=%DDBBASE%

ddb.exe %*