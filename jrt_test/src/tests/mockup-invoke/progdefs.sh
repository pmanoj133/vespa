#!/bin/bash
# Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
set -e
prog server 1 "tcp/$PORT_0" "./jrt_test_mockup-server_app"
