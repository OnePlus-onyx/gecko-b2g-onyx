# script to pull and import Firefox Translations's extension source code

import os.path
from zipfile import ZipFile
import subprocess
import shutil
import sys

if not os.path.exists("import_xpi.py"):
    sys.exit("This script is intended to be executed from its local folder")

have_xpi = "N"
local_xpi_file = (
    "bergamot-browser-extension-src/dist/production/firefox/"
    "firefox-infobar-ui/firefox-translations-0.4.0.xpi"
)
if os.path.isfile(local_xpi_file):
    have_xpi = input(
        "Extension xpi exists. Press Y to use it or any other key to rebuild it."
    )

if have_xpi.lower() != "y":
    # deleting old files if any
    shutil.rmtree("bergamot-browser-extension-src", ignore_errors=True)
    # cloning the extension
    subprocess.call(
        (
            "git clone -b v0.4.0 "
            "https://github.com/mozilla-extensions/bergamot-browser-extension/ "
            "bergamot-browser-extension-src "
        ).split()
    )
    # setting up the repo
    subprocess.call("yarn install".split(), cwd="bergamot-browser-extension-src")
    # pulling bergamot-translator submodule, the repo containing the port of the
    # neural machine translation engine to wasm
    subprocess.call(
        "git submodule update --init --recursive".split(),
        cwd="bergamot-browser-extension-src",
    )
    # build the wasm nmt module
    subprocess.call(
        "./bergamot-translator/build-wasm.sh".split(),
        cwd="bergamot-browser-extension-src",
    )
    # import the generated wasm module to the extension
    subprocess.call(
        "./import-bergamot-translator.sh ./bergamot-translator/build-wasm/".split(),
        cwd="bergamot-browser-extension-src",
    )
    # build the final xpi
    subprocess.call(
        "yarn build:firefox-infobar-ui".split(), cwd="bergamot-browser-extension-src"
    )

shutil.rmtree("extension", ignore_errors=True)
os.mkdir("extension")
file_exceptions = [
    "META-INF",
    ".md",
    "BRANCH",
    "COMMITHASH",
    "LASTCOMMITDATETIME",
    "VERSION",
    ".map",
    ".yaml",
]

fo = open("extension/jar.mn", "w")
fo.write(
    "##### This file was automatically generated by the import_xpi.py script ####\n"
)
fo.write("# This Source Code Form is subject to the terms of the Mozilla Public\n")
fo.write("# License, v. 2.0. If a copy of the MPL was not distributed with this\n")
fo.write("# file, You can obtain one at http://mozilla.org/MPL/2.0/.\n\n")
fo.write("browser.jar:\n")
fo.write("%   resource builtin-addons %builtin-addons/  contentaccessible=yes\n")
fo.write("    builtin-addons/translations/ (**)")
fo.write("\n")
fo.close()


def isValidFile(filename):
    for exception in file_exceptions:
        if exception in filename:
            return False
    return True


file_set = set()
# read xpi files
with ZipFile(local_xpi_file, "r") as zip:
    namelist = zip.namelist()
    cleared_namelist = []
    for filename in namelist:
        if isValidFile(filename):
            full_file_path = zip.extract(filename, "extension")
            if filename.endswith(".js"):
                filename = "browser/extensions/translations/{}".format(full_file_path)
                subprocess.call(
                    str(
                        "./mach lint --linter license {} --fix".format(filename)
                    ).split(),
                    cwd="../../../",
                )

print("Import finalized successfully")
