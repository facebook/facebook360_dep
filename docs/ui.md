---
id: ui
title: User Interface
---

In Mac/Windows Docker Desktop must be running, and enough CPUs and Memory should be given to the Docker Engine via Preferences -> Advanced.
Whenever the UI is run from a new terminal, the virtualenv in which all the Python dependencies were installed must be reactivated:

- Mac/Linux: `source ~/.venv/bin/activate`
- Windows: `~/.venv/Scripts/activate`

For Linux/Mac, it is easiest to have this added to `~/.bashrc`.

## Local rendering setup

The data to render must be in a directory with the following file structure:
```
<PROJECT>
├── background
│   └── color
│       ├── cam0
│       │   └── 000000.tif
│       ├── ...
│       └── camN
│           └── 000000.tif
├── rigs
│   └── rig.json
└── video
    └── color
        ├── cam0
        │   ├── 000001.tif
        │   ├── ...
        │   └── 000999.tif
        ├── ...
        └── camN
            ├── 000001.tif
            ├── ...
            └── 000999.tif
```
- `background`/`video`: background and color frames for each camera respectively. Note that `background` is optional, and frame images can have any supported file format.
- `rigs`: contains the uncalibrated rig json file that comes with the captured data.

To start a local render, make a copy of the file `res/flags/run.flags` and update the copy to set the following path:
~~~~
--project_root=<PROJECT>
~~~~
where `<PROJECT>` is the path to the project root.

NOTE for Windows users: all the paths in the flags file should have forward slashes ("/") instead of backslashes ("\\").


## Cloud (AWS) rendering setup

For cloud rendering we use Amazon Web Services (AWS). The UI will spawn EC2 instances, so whoever runs the software must be aware of the on-demand costs and the EC2 service limits for any instance type that is planned on being used:

https://aws.amazon.com/ec2/pricing/on-demand/
https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-resource-limits.html


The data to render must be in an Amazon S3 bucket with the following file structure:
```
<PROJECT>
├── background
│   └── color
│       └── 000000.tar
├── rigs
│   └── rig.json
└── video
    └── color
        ├── 000000.tar
        ├── ...
        └── 000999.tar
```
- `000000.tar`: archived version of `cam[0..N]/000000.tif`. Archived files are used to minimize the network requests and improve the I/O.


Before opening the GUI, the `<PROJECT>` directory should be in an S3 bucket, and an AWS access key must be created.
This is a .csv file containing the user's AWS access key ID and secret access key:
https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_access-keys.html

To render on AWS, make a copy of the file `res/flags/run.flags` and update the copy to set the following flags:
~~~~
--project_root=<S3_PROJECT>
--csv_path=<AWS_CREDENTIALS>
--cache=<LOCAL_CACHE_DIR>
~~~~
where `<S3_PROJECT>` is the path to the project root in S3, e.g, `s3://<bucket_name>/<some_path>/<PROJECT>`, and `<AWS_CREDENTIALS>` is the .csv file from AWS.

`<LOCAL_CACHE_DIR>` is an absolute path to a local directory used to store sample frames from `<S3_PROJECT>` for local processing. Following the previous example, if `<LOCAL_CACHE_DIR>` is `C:/Users/foo/data` it will generate the directory `C:/Users/foo/data/<some_path>/<PROJECT>`. Note that write access is required in
the directory specified in `<LOCAL_CACHE_DIR>`.

# Running the UI
To run the UI, open a Terminal and run:

- Mac/Linux
~~~~
cd <FACEBOOK360_DEP_ROOT>
python3 -u scripts/render/run.py --flagfile res/flags/<FLAGS_FILE>.flags
~~~~

- Windows
~~~~
cd <FACEBOOK360_DEP_ROOT>
python -u scripts/render/run.py --flagfile res/flags/<FLAGS_FILE>.flags
~~~~

Note that the first run can take ~20 minutes to compile the Docker image. Successive runs will only compile incremental changes.

In order to export non-6DoF content (e.g. equirectangular images), Docker needs access to a GPU in the host. This is currently only supported when running the UI from a Linux host with an NVIDIA GPU, or when using AWS cloud rendering (note that this will require GPU instances, which tend to be more expensive).
If the host is not Linux but has a GPU, even if not NVIDIA, we provide the option to visualize fused 6DoF frames if the facebook360_dep code has been compiled locally (check README_build.md).
An extra flag is needed in the flags file:
~~~~
--local_bin=<LOCAL_BIN_DIR>
~~~~
where `<LOCAL_BIN_DIR>` is an absolute path pointing at the `bin` directory of the compiled code in the host.


## Advanced flags
- `--verbose=True`: prints debugging messages about the progress. Default: *False*
- `--s3_ignore_fullsize_color`: when `project_root` is an S3 path it does not try to download full-size images. Useful when not wanting to calibrate. Default: *False*
- `--s3_sample_frame`: when `project_root` is an S3 path it downloads this frame for local testing, e.g. "000326". If empty it defaults to the first available frame. Default: *(empty)*
