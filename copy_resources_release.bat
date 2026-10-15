xcopy "Resources\*" "%~1" /D /E /Y
xcopy "Deps\release\Detours\LICENSE.md" "%~1\LICENSES\Detours\" /D /Y
xcopy "Deps\release\rapidxml\license.txt" "%~1\LICENSES\rapidxml\" /D /Y
xcopy "Deps\release\easywsclient\COPYING" "%~1\LICENSES\easywsclient\" /D /Y
xcopy "Misc\THIRDPARTY.yml" "%~1\LICENSES\advancedfx\" /D /Y
xcopy "" "%~1\LICENSES\advancedfx\" /D /Y
xcopy "Shaders\build\*.acs" "%~1\resources\shaders\" /D /Y
py -3 "%~dp0\Tools\make_readme.py" "%~1"
