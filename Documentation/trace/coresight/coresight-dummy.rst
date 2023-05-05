.. SPDX-License-Identifier: GPL-2.0

=============================
Coresight Dummy Trace Module
=============================

    :Author:   Hao Zhang <quic_hazha@quicinc.com>
    :Date:     May 2023

Introduction
---------------------------

Coresight Dummy Trace Module is for the specific devices that kernel
don't have permission to access or configure, e.g., CoreSight TPDMs
on Qualcomm platforms. So there need driver to register dummy devices
as Coresight devices. It may also be used to define components that
may not have any programming interfaces (e.g, static links), so that
paths can be established in the driver. Provide Coresight API for
dummy device operations, such as enabling and disabling dummy devices.
Build the Coresight path for dummy sink or dummy source for debugging.

Config details
---------------------------

There are two types of nodes, dummy sink and dummy source. The nodes
should be observed at the below coresight path::

    ``/sys/bus/coresight/devices``.

e.g.::

    / $ ls -l /sys/bus/coresight/devices | grep dummy
    dummy_sink0 -> ../../../devices/platform/soc@0/soc@0:sink/dummy_sink0
    dummy_source0 -> ../../../devices/platform/soc@0/soc@0:source/dummy_source0
