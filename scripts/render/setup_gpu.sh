# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

if hash nvidia-docker 2>/dev/null; then
  echo "Found nvidia-docker"
else
  # Check if we have an NVIDIA GPU
  sudo apt-get install -y mesa-utils
  if glxinfo | grep -i "nvidia"; then
    echo "Installing nvidia drivers. It make take a few minutes..."
    sudo apt-get update
    sudo apt install -y build-essential gcc libc-dev
    sudo apt-get install -y linux-headers-$(uname -r)
    wget https://developer.nvidia.com/compute/cuda/10.1/Prod/local_installers/cuda_10.1.168_418.67_linux.run
    sudo sh cuda_10.1.168_418.67_linux.run --silent --driver --toolkit

    # Display GPU info
    nvidia-smi

    echo "Installing nvidia-docker..."

    # Add the package repositories
    curl -s -L https://nvidia.github.io/nvidia-docker/gpgkey | sudo apt-key add -
    distribution=$(. /etc/os-release;echo $ID$VERSION_ID)
    curl -s -L https://nvidia.github.io/nvidia-docker/$distribution/nvidia-docker.list | sudo tee /etc/apt/sources.list.d/nvidia-docker.list
    sudo apt-get update

    # Install nvidia-docker2 and reload the Docker daemon configuration
    sudo apt-get install -y nvidia-docker2
    sudo pkill -SIGHUP dockerd

    # To test nvidia-smi with the latest official CUDA image
    # sudo docker run --runtime=nvidia --rm nvidia/cuda:9.0-base nvidia-smi
  else
    echo "No NVIDIA GPU found. Skipping GPU setup..."
  fi
fi
