---
id: faqs
title: FAQs
---

Here is a list of common issues we have seen when testing the rendering pipeline. Feel free to add more to this
document if you encounter more and believe other people will benefit from knowing how you
resolved this issue!

## RabbitMQ "driver failed programming..."
This means you already have RabbitMQ running somewhere on your host, which either means
you have it directly running on your computer or you have another Docker container that is
running it. This causes an error, since we are trying to bind to an already allocated port.

### RabbitMQ is running on your host
Run:
~~~
ps -aux | grep rabbitmq
~~~

If this comes up with a result, kill all the processes referenced, by doing:
~~~
kill -9 <pid>
~~~

### Another container is running RabbitMQ
If the above didn't solve your problem, run:
~~~
docker ps
~~~

See if there are any containers running, and if there are, use:

~~~
docker rm -f <container_name>
~~~

## Accessing workers' logs
For debugging purposes, we have worker logs that are periodically pulled from your render
location, whether that be on AWS or locally. If you are trying to report an issue, this
log is crucial for us to help you! The logs (for both local and AWS renders) are located in

### Local
~~~
<project_root>/logs/Worker-<timestamp>.txt
~~~

### AWS
~~~
<project_root>/logs/Worker-<AWS-id>.txt
~~~

where `<AWS-id>` is the instance ID of the worker, which can be found in the EC2 dashboard.

## There's a stalled job on my RabbitMQ queue
We automatically flush the queue on new renders. But, if you wish to manually flush it,
ssh into your staging machine and run the following:

~~~
sudo service rabbitmq-server restart
~~~

## run.py dies without error message
This could be caused by any number of things. To narrow down the cause, run:

~~~
docker logs -f $(docker ps -n1 -q)
~~~

Sending that information will make it easier for us to diagnose and resolve the issue!

## How do I log into the render container?
The render job is run either on your staging machine or in the local container, depending
on your configuration.

### Local
~~~
docker exec -it $(docker ps -q) /bin/bash
~~~

### AWS
~~~
docker exec -it $(docker ps -a --format '{{.Names}}' | awk 'NR==1{print $1}') /bin/bash
ssh -i /key.pem ubuntu@<ec2_staging_ip>
~~~

Now find a pod to log into using:
~~~
kubectl get pods
~~~

Taking any pod ID (the first column), run:
~~~
kubectl exec -it <pod_id> /bin/bash
~~~

## When creating the staging machine, I get "Permission denied (public key)"
This usually has to do with some conflicting permissions issue and is resolved by
closing the UI, deleting the local AWS directory (`<project_root>/aws`), and starting the UI again.

## I stopped kubernetes before it validated the cluster
This means we need to manually delete it from the staging machine, since the Terminate button in
UI will be grayed out. ssh into your staging machine and run:

~~~
KOPS_STATE_BUCKET=<username>-facebook360-kops-state-store
export KOPS_CLUSTER_NAME=<username>.facebook360.k8s.local
export KOPS_STATE_STORE=s3://${KOPS_STATE_BUCKET}
kops delete cluster --yes
~~~

where `<username>` is the your AWS IAM user.
