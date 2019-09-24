---
id: data
title: Data
---

To start experimenting, we are providing some sample datasets to work with.
Note: Since these were recorded with high end cameras, the datasets are quite large.
We recommend using an external hard drive for storage of the data, if you plan on
rendering them locally.

## Data
There are two datasets provided here:
- **The Complex**: Shoot from an enclosed outdoor apartment complex.
- **Room Chat**: Indoor shoot of friends casually hanging out together.

Each of these has three buckets provided:
- **Single Frame**: This is primarily provided for local testing to avoid having to
download a several GB dataset to test your render modifications.
- **Rendered Single Frame**: A sample render of the frame to show what the output looks like.
- **Frame Sequence**: Provided for testing an entire render. We recommend performing this
test in the cloud to avoid having to deal with excessive render times or local disk space.
- **Rendered Frame Sequence**: A sample render of the frame sequence to show what the output looks like.

### The Complex
- **Single Frame**: s3://facebook360-dep-sample-data/the_complex/1_frame_unpacked
- **Rendered Single Frame**: s3://facebook360-dep-sample-data/the_complex/1_frame_unpacked_rendered
- **Frame Sequence**: s3://facebook360-dep-sample-data/the_complex/20_frames
- **Rendered Frame Sequence**: s3://facebook360-dep-sample-data/the_complex/20_frames_rendered

### Room Chat
- **Single Frame**: s3://facebook360-dep-sample-data/room_chat/1_frame_unpacked
- **Rendered Single Frame**: s3://facebook360-dep-sample-data/room_chat/1_frame_unpacked_rendered
- **Frame Sequence**: s3://facebook360-dep-sample-data/room_chat/50_frames
- **Rendered Frame Sequence**: s3://facebook360-dep-sample-data/room_chat/50_frames_rendered
