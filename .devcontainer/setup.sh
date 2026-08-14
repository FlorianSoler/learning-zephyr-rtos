#!/bin/bash
set -e
cd /workspaces/LearningZephyr

if [ ! -d ".west" ]; then
    echo "Initializing Zephyr workspace..."
    west init -l .
fi

echo "Updating Zephyr modules..."
west update

# Install Python requirements BEFORE running west extension commands
pip3 install -r /workspaces/zephyr/scripts/requirements.txt

# Now export the environment safely
west zephyr-export