# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# We must use the full path to aws, since non-interactive shells do not source ~/.bashrc
aws configure set aws_access_key_id $1
aws configure set aws_secret_access_key $2
aws configure set default.region $3

KOPS_STATE_BUCKET=$6-facebook360-dep-kops-state-store
export KOPS_CLUSTER_NAME=$6.facebook360.dep.k8s.local
export KOPS_STATE_STORE=s3://${KOPS_STATE_BUCKET}

aws s3api create-bucket --bucket ${KOPS_STATE_BUCKET} --region us-west-2 --create-bucket-configuration LocationConstraint=us-west-2 &> /dev/null
aws s3api put-bucket-versioning --bucket ${KOPS_STATE_BUCKET} --versioning-configuration Status=Enabled

kops create secret --name ${KOPS_CLUSTER_NAME} sshpublickey admin -i ~/.ssh/id_rsa.pub

if $7; then
  echo "Deleting old cluster. This can take a few minutes..."
  kops delete cluster --yes
fi

kops create cluster \
  --node-count=$4 \
  --master-size=c4.large \
  --node-size=$5 \
  --zones=us-west-2a \
  --image kope.io/k8s-1.12-debian-stretch-amd64-hvm-ebs-2019-05-13 \
  --kubernetes-version 1.12.0 &> /dev/null

# Enable Kops Installation Hook and DevicePlugins for Nvidia GPUs
if [[ $5 == p* ]] || [[ $5 == g* ]]; then
  kops get ig nodes -o yaml > scripts/aws/ig.yml
  echo "  hooks:" >> scripts/aws/ig.yml
  echo "  - execContainer:" >> scripts/aws/ig.yml
  echo "      image: dcwangmit01/nvidia-device-plugin:0.1.0" >> scripts/aws/ig.yml
  kops replace -f scripts/aws/ig.yml
fi

kops update cluster --name ${KOPS_CLUSTER_NAME} --yes &> /dev/null

echo "Spawning $(($4 + 1)) machines. This can take several minutes..."
while [ 1 ]
do
    kops validate cluster && break || (sleep 1 && echo -e "\nStill initializing...\n" && sleep 10)
done

# Deploy the Daemonset for the Nvidia DevicePlugin
if [[ $5 == p* ]] || [[ $5 == g* ]]; then
  error_msg=$(kubectl create -f https://raw.githubusercontent.com/NVIDIA/k8s-device-plugin/v1.12/nvidia-device-plugin.yml 2>1&)
  if [[ ${error_msg} == *AlreadyExists*  ]]; then
    exit 0
  fi
fi
