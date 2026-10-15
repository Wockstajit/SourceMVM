@echo Requires GNU gettext for Windows to be installed.

echo "Project: hlae"
cd HLAE
mkdir ..\Deps\release\l10n\locales\en-US\hlae\
xgettext -k -kGetString -kGetPluralString:1,2 -kGetParticularString:1c,2 -kGetParticularPluralString:1c,2,3 -k_ -k_n:1,2 -k_p:1c,2 -k_pn:1c,2,3 -f xgettext-files.txt -L C# --from-code UTF-8 -o ..\Deps\release\l10n\locales\en-US\hlae\messages.pot "--copyright-holder=advancedfx.org" "--package-name=hlae" 
cd ..

@echo "Installing Python 3 polib ..."
py -3 -m pip install polib 

@echo "Project : Installer\shared\Dependency"
cd Installer\shared\Dependency
mkdir ..\..\..\Deps\release\l10n\locales\en-US\installer\shared\Dependency\
py -3 ..\..\..\Deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/en-US.wxl ..\..\..\Deps\release\l10n\locales\en-US\installer\shared\Dependency\messages.pot
cd ..\..\..

@echo "Project : Installer\HlaeFfmpeg"
cd Installer\HlaeFfmpeg
mkdir ..\..\Deps\release\l10n\locales\en-US\installer\HlaeFfmpeg\
py -3 ..\..\Deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/en-US.wxl ..\..\Deps\release\l10n\locales\en-US\installer\HlaeFfmpeg\messages.pot
cd ..\..

echo "Project : Installer\HlaeCore"
cd Installer\HlaeCore
mkdir ..\..\Deps\release\l10n\locales\en-US\installer\HlaeCore\
py -3 ..\..\Deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/en-US.wxl ..\..\Deps\release\l10n\locales\en-US\installer\HlaeCore\messages.pot
cd ..\..


echo "Project : Installer\setup"
cd Installer\setup
mkdir ..\..\Deps\release\l10n\locales\en-US\installer\setup\
py -3 ..\..\Deps\dev\wxl-po-tools\wxl2pot.py -f -l LangId lang/HyperlinkTheme_en-US.wxl ..\..\Deps\release\l10n\locales\en-US\installer\setup\messages.pot
cd ..\..
