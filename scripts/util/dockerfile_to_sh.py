#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Converts input Dockerfile to a bash script.

The generated bash script can be used to install dependencies and compile our
source code in the local host. It assumes there is a Dockerfile file in the current directory

Example:
    $ python3 dockerfile_to_sh.py
"""

import os
import re
import stat

if __name__ == "__main__":
    dockerfile_fn = "Dockerfile"
    if not os.path.isfile(dockerfile_fn):
        print(f"Cannot find {dockerfile_fn} in current directory")
        exit()
    with open(dockerfile_fn) as f:
        lines = f.readlines()

        # Concatenate split lines (ending with "\")
        newline_split = "\\\n"
        lines_no_split = [l.replace(newline_split, "").lstrip(" ") for l in lines]
        lines_clean = "".join(lines_no_split).splitlines()

    sh_fn = f"{dockerfile_fn}.sh"
    with open(sh_fn, "wt") as fout:
        fout.write("#!/bin/bash\n")
        run_delim = "RUN "
        for line in lines_clean:
            if line.startswith(("CMD", "COPY", "FROM", "WORKDIR")):
                continue
            elif any(s in line.lower() for s in ["docker", "rabbitmq"]):
                continue
            elif line.startswith(run_delim):
                line = line[len(run_delim)]
                if line.startswith('echo "Installing '):
                    m = re.search('Installing (.*)..."$', line)
                    pkg = m.group(1)
                    line = f"if [[ ! -f {pkg}.done ]]; then\n{line}"
                elif line.startswith('echo "Installed '):
                    m = re.search('Installed (.*)"$', line)
                    pkg = m.group(1)
                    check = f"""if [ $? -eq 0 ]; then \
                                touch {pkg}.done; \
                                else echo \"Failed to install {pkg}!\"; \
                                exit 1; \
                                fi"""
                    line = f"{' '.join(check.split())}\n{line}\nfi"
            elif line.startswith("ENV"):
                envs = line.split(" ")
                line = f"export {envs[1]}={envs[2]}"

            fout.write(f"{line}\n")

    os.chmod(sh_fn, os.stat(sh_fn).st_mode | stat.S_IEXEC)
