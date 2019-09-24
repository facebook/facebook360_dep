---
id: install
title: Get Started
---

## Docker (Recommended)

Download and install Docker for your platform first.
Execute the steps below from a Terminal to install the dependencies to run the user interface.
`<FACEBOOK360_DEP_ROOT>` is the path where the project is located.

### Mac
1. Install Docker: https://www.docker.com/products/docker-desktop
2. Install Homebrew (https://brew.sh/)
`/usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"`
3. Install XQuartz
`brew cask install xquartz`
4. Open XQuartz and Go to Preferences -> Security tab and make sure Allow connections from network clients is selected
5. Install python3
`brew install python`
6. Install dependencies
~~~~
cd ~
python3.7 -m venv ~/.venv
source ~/.venv/bin/activate
cd <FACEBOOK360_DEP_ROOT>
pip install -r requirements.txt
~~~~

### Windows
1. Install Docker: https://www.docker.com/products/docker-desktop
2. Install chocolatey: https://chocolatey.org/
3. Allow local PowerShell scripts to be run. Open PowerShell and run:
`Set-ExecutionPolicy RemoteSigned`
4. Install X Server:
`choco install vcxsrv`
5. Run Xlaunch from the start menu and follow initial configuration steps.
Check "Disable access control" on the Extra Settings window and click on "Save configuration" before clicking on "Finish". Save the configuration on the Desktop.
6. Install python3
`choco install python --version 3.7.2 -y`
7. Install dependencies
~~~~
cd ~
python -m venv .venv
.venv/Scripts/activate
cd <FACEBOOK360_DEP_ROOT>
pip install -r requirements.txt
~~~~

### Linux
1. Install Docker: https://docs.docker.com/install/linux/docker-ce/ubuntu/
2. Install pip3
`sudo apt install -y python3-pip`
3. Install dependencies
~~~~
cd ~
python3 -m venv ~/.venv
source ~/.venv/bin/activate
cd <FACEBOOK360_DEP_ROOT>
pip install -r requirements.txt
~~~~

## Standalone (Not Recommended)

Execute the steps below from a Terminal, and from the root of the project:

### Linux
We can use the steps straight from Dockerfile:
~~~~
python3 scripts/util/docker_to_sh.py
sudo ./Dockerfile.sh
~~~~

### MacOS
`Homebrew` handles all the dependencies, so we don't have to install them from source.

1. Install Homebrew (https://brew.sh/)
`/usr/bin/ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"`
2. Make sure system paths have the right permissions
~~~~
sudo mkdir -p /usr/local/Frameworks && \
sudo chown -R $(whoami) /usr/local/lib /usr/local/sbin
~~~~
3. Install dependencies
~~~~
brew install cmake automake autoconf python gflags glog && \
brew install double-conversion && brew link double-conversion --force && \
brew install boost ceres-solver eigen folly && \
brew install opencv@3 && brew link opencv@3 --force && \
brew install qt5 glew glfw3 PyQt5 && \
pip3 install watchdog absl-py pika docker progressbar boto3 fabric patchwork netifaces \
brew install rabbitmq && brew services start rabbitmq
~~~~
(Install gtest)
~~~~
git clone https://github.com/google/googletest
cd googletest
mkdir build
cd build
cmake ..
make
make install
~~~~
4. Enabling multithreaded ceres (Optional)

To enable multithreading on MacOS, ceres needs to be rebuilt using different flags. If you installed from brew, open the ceres formula
~~~~
brew edit ceres-solver
~~~~
Modify the cmake command by adding the following flags below `DGLOG_LIBRARY_DIR_HINTS`
~~~~
"-DOPENMP=OFF",
"-DCXX11=ON",
"-DCXX11_THREADS=ON"
~~~~
Rebuild ceres from source:
~~~~
brew reinstall --build-from-source ceres-solver
~~~~
5. GLFW3.3
~~~~
git clone https://github.com/glfw/glfw.git && \
cd glfw && git checkout 3.3 && \
mkdir build && cd build && cmake .. && make -j && sudo make install -j && \
cd ../.. && rm -rf glfw
~~~~
6. Compile facebook360_dep code
~~~~
mkdir build && cd build && \
cmake -DCMAKE_BUILD_TYPE=Release .. && \
make -j
~~~~
If using XCode:
~~~~
mkdir xcode && cd xcode
cmake -G Xcode ..
~~~~
will create `xcode/Facebook360_DEP.xcodeproj`
