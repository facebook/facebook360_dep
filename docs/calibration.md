---
id: calibration
title: Calibration
---

In order to produce a more accurate and comfortable result in VR, the Facebook360 Depth Estimation Pipeline uses several calibration config files to correct optical and mechanical issues. This document describes the process of generating the calibration config files.

WARNING: you should not attempt to render videos captured with Facebook360 Depth Estimation Pipeline without first reading this document. Uncalibrated results may be severely distorted in VR to the point of breaking stereo perception of 3D.

## Geometric Calibration

No matter how well constructed the rig is, our software needs to know the geometric properties of the cameras (intrinsic and extrinsic) in order to accurately perform stereo reconstruction.

The steps below describe the geometric calibration process for a camera rig.

* Capture a single frame in a scene with plenty of features, i.e. containing objects with sharp edges and corners of different sizes. A good example is the interior of an office.

* Unpack the frames and place the RGB images for each camera in a separate directory. For this example we assume they are in ~/Desktop/geometric_calibration/rgb/cam[0-15]/000000.png.

* Add the corresponding rig file to ~/Desktop/geometric_calibration/rig.json

* Go to facebook360_dep and run the following command:
~~~
Calibration \
--color=~/Desktop/geometric_calibration/rgb \
--matches=output_folder/matches.json \
--rig_in=~/Desktop/geometric_calibration/rig.json \
--rig_out=output_folder/rig_calibrated.json \
--frame=000000
~~~

* This generates a new JSON file, rig_calibrated.json, to be used when rendering.
