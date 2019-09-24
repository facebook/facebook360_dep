# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Install docker
sudo apt-get install -y apt-transport-https ca-certificates curl software-properties-common
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo apt-key add -
sudo add-apt-repository "deb [arch=amd64] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable"
sudo apt-get -y update
sudo apt-get install -y docker-ce
sudo usermod -a -G docker $USER

# Install jq
sudo apt-get install -y jq

# Install aws
sudo DEBIAN_FRONTEND=noninteractive apt-get install -yq python3-pip
pip3 install pika docker absl-py progressbar awscli --upgrade --user
export PATH=~/.local/bin:$PATH

# Install kubectl
sudo apt-get update && sudo apt-get install -y apt-transport-https
curl -s https://packages.cloud.google.com/apt/doc/apt-key.gpg | sudo apt-key add -
sudo touch /etc/apt/sources.list.d/kubernetes.list
echo "deb http://apt.kubernetes.io/ kubernetes-xenial main" | sudo tee -a /etc/apt/sources.list.d/kubernetes.list
sudo apt-get update
sudo apt-get install -y kubectl

# Install heptio auth
curl -o heptio-authenticator-aws https://amazon-eks.s3-us-west-2.amazonaws.com/1.10.3/2018-06-05/bin/linux/amd64/heptio-authenticator-aws
chmod +x ./heptio-authenticator-aws
mkdir -p ~/bin
cp heptio-authenticator-aws ~/bin
echo 'export PATH=$HOME/bin:$PATH' >> ~/.bashrc
export PATH=$HOME/bin:$PATH

# Install helm
wget https://storage.googleapis.com/kubernetes-helm/helm-v2.9.1-linux-amd64.tar.gz
tar -zxf helm-v2.9.1-linux-amd64.tar.gz
cp linux-amd64/helm ~/bin
helm init

# Install helm repos
# See https://github.com/kubernetes/helm/issues/3130
kubectl create serviceaccount --namespace kube-system tiller
kubectl create clusterrolebinding tiller-cluster-rule --clusterrole=cluster-admin --serviceaccount=kube-system:tiller
kubectl patch deploy --namespace kube-system tiller-deploy -p '{"spec":{"template":{"spec":{"serviceAccount":"tiller"}}}}'
helm repo add incubator https://kubernetes-charts-incubator.storage.googleapis.com/

# Install RabbitMQ
sudo apt-get install -y rabbitmq-server
sudo bash -c 'echo "[{rabbit, [{loopback_users, []}]}]." > /etc/rabbitmq/rabbitmq.config'
sudo service rabbitmq-server restart

# Install Kops (Kubernetes setup client)
wget https://github.com/kubernetes/kops/releases/download/1.10.0/kops-linux-amd64
chmod +x kops-linux-amd64
sudo mv kops-linux-amd64 /usr/local/bin/kops

# Create SSH keypair for future Kubernetes deployments
yes y | ssh-keygen -q -t rsa -N '' -f ~/.ssh/id_rsa >/dev/null
