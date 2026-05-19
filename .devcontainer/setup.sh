#!/bin/bash
set -e

cd /workspaces/zephyrproject

if [ ! -d ".west" ]; then
    west init -l .
fi

west update
west zephyr-export

pip3 install -r /opt/zephyrproject/zephyr/scripts/requirements.txt

mkdir -p build

# Generate compile_commands.json for IntelliSense
west build -b native_sim samples/hello_world -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ln -sf build/compile_commands.json compile_commands.json

echo "Zephyr dev environment ready"