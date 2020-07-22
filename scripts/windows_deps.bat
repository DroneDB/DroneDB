:: Check if cmake-js needs to be installed along with mocha for unit tests
for %%X in (cmake-js) do (set FOUND-CMAKEJS=%%~$PATH:X)
if NOT DEFINED FOUND-CMAKEJS npm install -g cmake-js mocha
