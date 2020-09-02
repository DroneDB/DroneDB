echo Copying DLL to output directory

set TARGETDIR=%1
if "%DDB_LIB_PATH%"=="" (SET COPY_FROM=..\..\build) ELSE (SET COPY_FROM="%DDB_LIB_PATH%") 

if NOT EXIST "%COPY_FROM%\ddb.dll" GOTO :NODIR

xcopy "%COPY_FROM%\*.dll" "%TARGETDIR%" /i /d /y
xcopy "%COPY_FROM%\*.sqlite" "%TARGETDIR%" /i /d /y
xcopy "%COPY_FROM%\*.bin" "%TARGETDIR%" /i /d /y
xcopy "%COPY_FROM%\*.db" "%TARGETDIR%" /i /d /y
xcopy "%COPY_FROM%\*.exe" "%TARGETDIR%" /i /d /y
xcopy "%COPY_FROM%\*.bat" "%TARGETDIR%" /i /d /y

exit 0

:NODIR
echo Error! No ddb.dll found in %COPY_FROM%. Build ddb.dll first, then try again. You can also set DDB_LIB_PATH to a different path if you've built the library elsewhere.
exit 1