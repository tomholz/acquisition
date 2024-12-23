#!/bin/bash

# Copyright (C) 2014-2024 Acquisition Contributors
#
# This file is part of Acquisition.
#
# Acquisition is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# Acquisition is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with Acquisition.  If not, see <http://www.gnu.org/licenses/>.

MACDEPLOYQT=~/Qt/6.8.1/macos/bin/macdeployqt

TARGET=./build/Qt_6_8_1_for_macOS-Release

pushd $TARGET

$MACDEPLOYQT acquisition.app -verbose=2 -always-overwrite -appstore-compliant -dmg

popd
