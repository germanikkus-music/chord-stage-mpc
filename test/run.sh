#!/bin/sh
# Builds the scripted render test against the native plugin build and runs it.
# Run from the project folder after: python3 ../mpc-plugin-builder-sdk/tools/build.py .
set -e
cd "$(dirname "$0")/.."
SDK=../mpc-plugin-builder-sdk
c++ -std=c++17 -O2 -I"$SDK/framework" -o build/script_test test/script_test.cpp
./build/script_test build/native/chord-stage.dylib build/script.wav
