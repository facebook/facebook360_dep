# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Flushes the queue from any previous (incomplete) runs
sudo service rabbitmq-server restart

# Allows access to AWS secretes
if [ $(kubectl get secrets | grep aws-storage-key | wc -l) > 0 ]; then
  kubectl delete secrets aws-storage-key
fi

kubectl create secret generic aws-storage-key \
  --from-literal=AWS_ACCESS_KEY_ID=$(aws configure get aws_access_key_id) \
  --from-literal=AWS_SECRET_ACCESS_KEY=$(aws configure get aws_secret_access_key)

# Get k8s cluster ready
sed "s|<REPO_NAME>|$1:worker|g" scripts/aws/farm.yml.template > scripts/aws/farm.yml
sed -i "s|<MASTER_IP>|$2|g" scripts/aws/farm.yml
sed -i "s|<WORKER_COUNT>|$3|g" scripts/aws/farm.yml
kubectl apply -f scripts/aws/farm.yml

# Ensure that all the pods receive the newest image
kubectl delete pods --all --grace-period=0 --force
