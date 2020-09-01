# Copyright 2004-present Facebook. All Rights Reserved.

# This software is proprietary and confidential to Facebook, Inc.  Any reproduction, distribution,
# modification, performance, display, and other use or disclosure is prohibited except to the
# extent expressly permitted in a written agreement executed by an authorized representative of
# Facebook, Inc.

FROM ubuntu:18.04
WORKDIR /app/deps

RUN apt-get update && apt-get install -y \
    build-essential software-properties-common \
    libxmu-dev libxi-dev libflann-dev wget libatlas-base-dev unzip \
    curl git python3-pip gfortran nano vim libgles2-mesa-dev libsuitesparse-dev

RUN apt-get update && apt-get install -y && \
    add-apt-repository ppa:deadsnakes/ppa -y && \
    apt install -y python3.7 python3.7-dev
RUN echo "alias python3=python3.7" >> ~/.bash_aliases && \
    echo "alias python=python3" >> ~/.bash_aliases && \
    curl -LO https://bootstrap.pypa.io/get-pip.py && python3.7 get-pip.py && rm get-pip.py && \
    echo "alias pip3=pip3.7" >> ~/.bash_aliases && \
    ln -fs python3.7 /usr/bin/python3
RUN bash -c 'source ~/.bash_aliases'

RUN echo "Installing cmake..."
RUN curl -LO https://cmake.org/files/v3.11/cmake-3.11.4.tar.gz && \
    tar -zxf cmake-3.11.4.tar.gz && \
    cd cmake-3.11.4 && \
    ./bootstrap && make -j$(nproc) && make install -j$(nproc) && \
    cd ../ && rm -fr cmake-3*
RUN echo "Installed cmake"

RUN echo "Installing gflags..."
RUN curl -LO https://github.com/gflags/gflags/archive/v2.2.1.zip && \
    unzip v2.2.1.zip && mkdir gflags-2.2.1/build && cd gflags-2.2.1/build && \
    cmake -DBUILD_SHARED_LIBS:BOOL=ON .. && \
    make -j$(nproc) && make install -j$(nproc) && \
    cd ../.. && rm -rf v2.2.1.zip gflags-2.2.1
RUN echo "Installed gflags"

RUN echo "Installing glog..."
RUN curl -LO https://github.com/google/glog/archive/v0.3.5.zip && \
    unzip v0.3.5.zip && cd glog-0.3.5 && \
    mkdir build && cd build && \
    cmake -H. -Bbuild -G "Unix Makefiles" .. && cd .. && \
    cmake --build build && cmake --build build --target install && \
    cd .. && rm -rf v0.3.5.zip glog-0.3.5
RUN echo "Installed glog"

RUN echo "Installing double-conversion..."
RUN git clone https://github.com/google/double-conversion.git && \
    cd double-conversion && git checkout v3.0.3 && \
    mkdir build && cd build && \
    cmake .. && make -j$(nproc) && make install -j$(nproc) && \
    cd ../.. && rm -rf double-conversion
RUN echo "Installed double-conversion"

RUN echo "Installing boost..."
RUN curl -LO https://dl.bintray.com/boostorg/release/1.66.0/source/boost_1_66_0.tar.gz && \
    tar -zxf boost_1_66_0.tar.gz && \
    cd boost_1_66_0 && \
    ./bootstrap.sh && \
    ./b2 install --layout=tagged -j$(nproc) && \
    cd .. && rm -fr boost_1_66_0
RUN echo "Installed boost"

RUN echo "Installing eigen..."
RUN curl -LO https://gitlab.com/libeigen/eigen/-/archive/3.3.4/eigen-3.3.4.tar.gz && \
    tar -zxf eigen-3.3.4.tar.gz && \
    cd eigen-3.3.4 && \
    mkdir build && cd build && \
    cmake .. && make install -j$(nproc) && \
    cd ../../ && rm -fr eigen-3.3.4
RUN echo "Installed eigen"

RUN echo "Installing ceres..."
RUN curl -LO http://ceres-solver.org/ceres-solver-1.14.0.tar.gz && \
    tar zxf ceres-solver-1.14.0.tar.gz && \
    mkdir ceres-bin && \
    cd ceres-bin && \
    cmake ../ceres-solver-1.14.0 && \
    make -j$(nproc) && \
    make install && \
    cd .. && rm -fr ceres*
RUN echo "Installed ceres"

RUN echo "Installing folly..."
RUN apt-get update && apt-get install -y \
    g++ cmake libevent-dev libdouble-conversion-dev libgoogle-glog-dev \
    libgflags-dev libiberty-dev liblz4-dev liblzma-dev libsnappy-dev make \
    zlib1g-dev binutils-dev libjemalloc-dev libssl-dev pkg-config
RUN git clone https://github.com/facebook/folly.git && \
    cd folly && git checkout v2018.08.06.00 && \
    mkdir _build && cd _build && \
    cmake configure .. -DBUILD_SHARED_LIBS:BOOL=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON && \
    make -j$(nproc) && \
    make install -j$(nproc) && \
    cd ../.. && rm -fr folly
RUN echo "Installed folly"

RUN echo "Installing glfw..."
RUN apt-get update && apt-get install -y \
    libglfw3-dev libdmx-dev libdmx1 libxinerama-dev libxinerama1 libxcursor-dev libxcursor1 libosmesa6-dev
RUN git clone https://github.com/glfw/glfw.git && \
    cd glfw && git checkout 3.3 && \
    mkdir build && cd build && \
    cmake -DGLFW_USE_OSMESA=1 .. && make -j$(nproc) && make install -j$(nproc) && \
    cd ../.. && rm -rf glfw
RUN echo "Installed glfw"

RUN echo "Installing opencv..."
RUN apt-get update && apt-get install -y \
    libjpeg-dev libpng-dev libtiff-dev \
    libavcodec-dev libavformat-dev libswscale-dev libv4l-dev \
    libxvidcore-dev libx264-dev \
    libgtk-3-dev libatlas-base-dev gfortran
RUN pip3 install numpy
RUN git clone -b 3.4.3 https://github.com/opencv/opencv --depth 1 && \
    cd opencv && mkdir build_ && cd build_ && \
    cmake -D CMAKE_BUILD_TYPE=Release \
    -D BUILD_TESTS=OFF -D BUILD_PERF_TESTS=OFF -D ENABLE_FAST_MATH=1 \
    -D PYTHON_EXECUTABLE=/usr/bin/python3 \
    .. && \
    make install -j$(nproc) && \
    cd ../.. && rm -rf opencv*
RUN echo "Installed opencv"

RUN echo "Installing gtest..."
RUN git clone https://github.com/google/googletest && \
    cd googletest && mkdir build_ && cd build_ && \
    cmake .. && make -j$(nproc) && make install -j$(nproc) && \
    cd ../.. && rm -rf googletest
RUN echo "Installed gtest"

# Dependencies for render
RUN apt-get update && apt-get install -y \
    cifs-utils qt5-default iputils-ping net-tools rsync \
    apt-transport-https ca-certificates curl lxc iptables
RUN apt-get update && apt-get install -y rabbitmq-server docker

# Install Docker
RUN curl -sSL https://get.docker.com/ | sh

# Install Python dependencies
RUN pip3 install pika qdarkstyle absl-py awscli watchdog progressbar \
    patchwork netifaces pyqt5 boto3 pyvidia cryptography docker networkx wget --upgrade
RUN apt-get update && apt-get install -y libglew-dev freeglut3-dev pciutils

# Install imageio (needs to be done after the above dependencies)
RUN pip3 install imageio

# Build source
WORKDIR /app/facebook360_dep
COPY source ./source
COPY CMakeLists.txt ./CMakeLists.txt
COPY ISPC.cmake ./ISPC.cmake
RUN mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
COPY scripts ./scripts
COPY res ./res
COPY Dockerfile ./Dockerfile

# Creates RabbitMQ server
RUN bash -c 'echo "[ {rabbit, [ {loopback_users, []} ]} ]." > /etc/rabbitmq/rabbitmq.config'

# Setup NVIDIA
ENV NVIDIA_VISIBLE_DEVICES ${NVIDIA_VISIBLE_DEVICES:-all}
ENV NVIDIA_DRIVER_CAPABILITIES ${NVIDIA_DRIVER_CAPABILITIES:+$NVIDIA_DRIVER_CAPABILITIES,}graphics,compat32,utility

COPY --from=nvidia/opengl:1.0-glvnd-runtime-ubuntu18.04 \
  /usr/share/glvnd/egl_vendor.d/10_nvidia.json \
  /usr/share/glvnd/egl_vendor.d/10_nvidia.json

RUN mkdir /tmp/runtime-root
ENV XDG_RUNTIME_DIR=/tmp/runtime-root

CMD tail -f /dev/null
