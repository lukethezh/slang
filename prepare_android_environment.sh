#!/bin/bash
cmake --preset vs2022
cmake --build --preset generators
cmake --install build --prefix generators --component generators
rm -rf build