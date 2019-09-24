---
id: api
title: API
sidebar_label: API
---

We automatically generate our API documentation with Doxygen:
- [Scripts (Python)](/scripts/html/)
- [Source (C++)](/source/html/)

For quick reference, here is a list of the binaries in our repository and descriptions of
their uses along with an example.

## AlignColors
Aligns colors using separate (calibrated) color rigs.
~~~
./AlignColors \
--first=000000 \
--last=000000 \
--output=/path/to/output \
--color=/path/to/video/color \
--calibrated_rig=/path/to/rigs/rig_calibrated.json \
--rig_blue=/path/to/rigs/rig_blue.json \
--rig_green=/path/to/rigs/rig_green.json \
--rig_red=/path/to/rigs/rig_red.json
~~~

## AlignPointCloud
Aligns point cloud to camera rig. The transformation includes translation, rotation and scaling.
~~~
./AlignPointCloud \
--color=/path/to/background/color \
--point_cloud=/path/to/lidar/points.pts \
--rig_in=/path/to/rigs/rig.json \
--rig_out=/path/to/rigs/rig_aligned.json
~~~

## Calibration
Calibrates an uncalibrated rig by feature matching and performing geometric calibration
on a sample frame.
~~~
./Calibration \
--color=/path/to/video/color \
--matches=/path/to/output/matches.json \
--rig_in=/path/to/rigs/rig.json \
--rig_out=/path/to/rigs/rig_calibrated.json \
--frame=000000
~~~

## CalibrationLibMain
Calibrates an uncalibrated rig by feature matching and performing geometric calibration
on a sample frame. Unlike Calibration, this app takes fixed command line arguments.
~~~
./CalibrationLibMain \
/path/to/rigs/rig_calibrated.json \
/path/to/output/matches.json \
/path/to/rigs/rig.json \
/path/to/video/color
~~~

## ComputeRephotographyErrors
Computes rephotography error for a set of frames. Rephotography error for a single frame is
computed by generating cubemaps for both the reference and the rendered data, translating the
cubemap origin to the center of the reference camera, and computing the MSSIM for each camera.
~~~
./ComputeRephotographyErrors \
--first=000000 \
--last=000000 \
--output=/path/to/output \
--rig=/path/to/rigs/rig.json \
--color=/path/to/video/color \
--disparity=/path/to/output/disparity
~~~

## ConvertToBinary
Expects all files to be in the format `<dir>/<camera>/<frame>.extension`

If <color> is specified:
Read .png files and save them as .rgba files in <bin> folder
If <disparity> is specified:
Read .pfm files and save them as .vtx and .idx files in <bin> folder

<bin> folder is created for each frame if it does not exist

If <rgba> is specified:
Convert color image into an RGBA binary stream

If <obj> is specified:
Read .vtx and .idx files from <bin> and save .obj files to <obj> folder
~~~
./ConvertToBinary \
--color=/path/to/video/color \
--rig=/path/to/rigs/rig.json \
--first=000000 \
--last=000000 \
--disparity=/path/to/output/disparity \
--bin=/path/to/output/bin \
--fused=/path/to/output/fused
~~~

## CreateObjFromDisparityEquirect
Creates an OBJ (optionally with texturing) from a disparity equirect.
~~~
./CreateObjFromDisparityEquirect \
--input_png_color=/path/to/equirects/color.png \
--input_png_disp=/path/to/equirects/disparity.png \
--output_obj=/path/to/output/test.obj
~~~

## DerpCLI
Runs depth estimation on a set of frames. We assume the inputs have already been resized into
the appropriate pyramid level widths before execution. See scripts/render/config.py to see
the assumed widths.
~~~
./DerpCLI \
--input_root=/path/to/ \
--output_root=/path/to/output \
--rig=/path/to/rigs/rig.json \
--first=000000 \
--last=000000
~~~

## GenerateCameraOverlaps
Generates a series of images of the rig cameras projected into destination cameras over
a series of fixed depths.
~~~
./GenerateCameraOverlaps \
--frame=000000 \
--output=/path/to/output \
--rig=/path/to/rigs/rig.json \
--color=/path/to/video/color

A typical extension of this is creating a video over the series of depth generated, i.e.:

ffmpeg -framerate 10 -pattern_type glob \
-i '/path/to/output/overlaps/cam0/*.png' -c:v libx264 -pix_fmt yuv420p \
-vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" /path/to/output/overlaps/cam0.mp4 -y
~~~

## GenerateEquirect
Generates an equirect from a set of color images at a uniformly spaced range of depths.
~~~
./GenerateEquirect \
--color=/path/to/video/color \
--output=/path/to/output \
--rig=/path/to/rigs/rig.json \
--frame=000000 \
--depth_min=1.0 \
--depth_max=1000.0 \
--num_depths=50
~~~

## GenerateForegroundMasks
Generates foreground masks for a series of frames assuming a fixed background. Various
parameters can be tweaked to improve the mask accuracy.
~~~
./GenerateForegroundMasks \
--first=000000 \
--last=000000 \
--rig=/path/to/rigs/rig.json \
--color=/path/to/video/color \
--background_color=/path/to/background/color \
--foreground_masks=/path/to/video/output
~~~

## GenerateKeypointProjections
Reprojects a grid of keypoints to another camera at different depths.
~~~
./GenerateKeypointProjections \
--rig=/path/to/rigs/rig.json \
--output_dir=/path/to/output
~~~

## GeometricCalibration
Performs geometric calibration on a sample frame. The results of the feature matcher should
be available before execution.
~~~
./GeometricCalibration \
--color=/path/to/video/color \
--matches=/path/to/output/matches.json \
--rig_in=/path/to/rigs/rig.json \
--rig_out=/path/to/rigs/rig_calibrated.json \
--frame=000000
~~~

## GeometricConsistency
Compute initial depth for every camera
Repeat pass_count times:
Clean away depths that are implausible
Recompute depths using clean depths to estimate occlusions
~~~
GeometricConsistency \
--color /path/to/color \
--output /path/to/output \
--rig /path/to/rigs/rig.json \
--first 000000 \
--last 000000
~~~

## GlViewer
OpenGL-based viewer for binary 6dof data files.

Keyboard navigation:
w, a, s, d as well as [, and ] will rotate the view.
Hold down shift to move the viewpoint instead.
z, and x move forward and backward.

Mouse navigation:
Drag the mouse to rotate. hold down shift to unlock the vertical axis.
Right button drag the mouse to pan. hold down shift to unlock the vertical axis.

Misc:
Hit 'r' to reset the view to what was on the command line.
Hit 'p' to dump the current view parameters in the command line format.
~~~
./GlViewer \
--rig=/path/to/output/fused/rig_calibrated.json \
--catalog=/path/to/output/fused/fused.json \
--disks=/path/to/output/fused/fused_0.bin
~~~

## LayerDisparities
Layers foreground disparity atop background disparity assuming nans to correspond to locations
without valid disparities.
~~~
./LayerDisparities \
--rig=/path/to/rigs/rig.json \
--background_disp=/path/to/background/disparity \
--foreground_disp=/path/to/output/disparity \
--output=/path/to/output \
--first=000000 \
--last=000000
~~~

## MatchCornersMain
Performs feature matching on a sample frame.
~~~
./MatchCorners \
--color=/path/to/video/color \
--matches=/path/to/output/matches.json \
--rig_in=/path/to/rigs/rig.json \
--frame=000000
~~~

## RawToRgb
Converts a RAW image to RGB using a given ISP configuration.
~~~
./RawToRgb \
--input_image_path=/path/to/video/color/000000.raw \
--output_image_path=/path/to/video/color/000000.png \
--isp_config_path=/path/to/video/isp.json
~~~

## RigAligner
Aligns the scale, position, and orientation of the input rig to a reference rig via rescaling,
translating, and rotating respectively. These can be selectively locked.
~~~
./RigAligner \
--rig_in=/path/to/rigs/rig.json \
--rig_reference=/path/to/rigs/reference.json \
--rig_out=/path/to/rigs/aligned.json
~~~

## RigAnalyzer
Miscellaneous analysis utilities for a rig. Various output formats are supported to
visualize the rig setup (e.g. equirect projection).
~~~
./RigAnalyzer \
--rig=/path/to/rigs/rig.json \
--output_equirect=/path/to/output/equirect.png
~~~

## RigCompare
Performs a camera-to-camera compare between an input rig and a reference rig.
~~~
./RigCompare \
--rig=/path/to/rigs/rig.json \
--reference=/path/to/rigs/reference.json
~~~

## RigSimulator
Render an artificial scene as seen by the specified rig.
~~~
./RigSimulator \
--mode=pinhole_ring \
--skybox_path=/path/to/skybox.png
~~~

## TemporalBilateralFilter
Runs temporal filter across disparity frames using corresponding color frames as guides.
~~~
./TemporalBilateralFilter \
--input_root=/path/to/ \
--output_root=/path/to/output \
--rig=/path/to/rigs/rig.json \
--first=000000 \
--last=000000
~~~
