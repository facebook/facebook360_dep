---
id: render
title: Render
---

## Overview
After running calibration, we render depth maps from each of the cameras, from which we can produce
a 3D scene that will be meshed and sent to the headset. The rendering pipeline works through a number
of stages, each involving one or more binaries, to produce its results:
- Precompute Resizes
- Generate Foreground Masks
- Precompute Resizes Foreground
- Depth Estimation
  - DerpCLI
  - Temporal Bilateral Filter
  - Upsample Disparity
- Convert To Binary
- Stripe Binaries
- Simple Mesh Renderer

There are a number of standard workflows we have run through the render pipeline described in the
[Workflows](/docs/workflow) section.
The user interface uses
this render pipeline for nearly all of its back-end functionality, with the exceptions of calibration and the viewer.

## Precompute Resizes
The crux of depth estimation uses a Gaussian Pyramid. Since it is likely thet the render is run over the
same data multiple times, the image levels are pre-computed before proceeding to depth estimation.

- **Ingest**:
  - Full-size color images per camera
- **Produces**:
  - Resized level color images per camera

## Generate Foreground Masks
Foreground masks are binary masks used to segment the foreground from the background to improve
efficiency and correctness of depth estimation.

- **Ingest**:
  - Color image for foreground and background per camera (Typically finest level of the resize)
- **Produces**:
  - Binary foreground mask image per camera

*Note*: In the case of producing disparity images higher than 2K pixels wide, the foreground masks should be generated at the desired resolution to improve results.

## Precompute Resizes Foreground
Similar to the Precompute Resizes step above, the foreground masks need to be resized so that they can
be used at each level. This step is ignored if no foreground masks are used.

- **Ingest**:
  - Full-size foreground masks per camera
- **Produces**:
  - Resized level foreground masks per camera

## Depth Estimation
This the crux of the pipeline, which takes all the precomputed results and produces disparity
maps. Disparities are defined as the reciprocal of depths (i.e. `1/depth`). At each level if performs depth estimation and applies a temporal filter to smooth the results. The finest level disparities are upsampled to the final resolution if necessary.

- **Ingest**:
  - All resized levels color images per camera
  - All resized levels level foreground masks per camera
- **Produces**:
  - Disparity map per camera

### DerpCLI (Single Level)
This stage produces disparity maps given overlapping color images. Multiple levels can be
performed in a single execution, although the standard render pipeline assumes separate runs
per level.

- **Ingest**:
  - Single resized level color images per camera
  - Single resized foreground masks per camera
- **Produces**:
  - Single level disparity map per camera

### Temporal Bilateral Filter (Single Level)
From the disparity map produced per camera, a temporal filter is applied to ensure smoothness
across frames.

- **Ingest**:
  - Single level disparity maps per camera in a contiguous temporal range
- **Produces**:
  - Single level disparity map per camera

### Upsample Disparity
If the desired resolution of the per-camera disparity maps is higher than 2048 pixels wide a separate stage performs upsampling using the color images as
a guide. This is only performed after completing the finest level of the pyramid.

- **Ingest**:
  - Single level disparity map per camera
- **Produces**:
  - Single level disparity map per camera


## Simple Mesh Renderer
For output that not expected to be viewed in 6DoF the render goes through
this stage, which produces other formats using the same inputs.

- **Ingest**:
  - Level 0 color image per camera
  - Level 0 disparity map per camera
- **Produces**:
  - One of the following formats:
    - Color cubemap
    - Disparity cubemap
    - Equirect color
    - Equirect disparity
    - Left-Right 180 stereo
    - Snapshot color
    - Snapshot disparity
    - Top bottom 3DoF
    - Top bottom stereo

## Convert To Binary
The disparity maps are converted into mesh files (i.e. vtx, idx, and bc7) for streaming. These formats are only necessary if the render is to be viewed in a headset.

- **Ingest**:
  - Level 0 color image per camera
  - Level 0 disparity map per camera
- **Produces**:
  - Binary files (idx, vtx, bc7) per camera

## Stripe Binaries
When viewing in a headset, the player expects to read from a single striped file rather than the
individual files produced in the binary conversion process. This produces the final .bin file and associated
metadata needed by the Rift viewer. We highly recommend doing this locally, since it is only performed on a
single computer and will eventually need to be downloaded locally for viewing purposes.

- **Ingest**:
  - Binary files (idx, vtx, bc7) per camera
- **Produces**:
  - Striped binary file
  - Metadata json corresponding to the binary
