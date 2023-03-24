.. SPDX-License-Identifier: GPL-2.0

=============================
Coresight Dummy Trace Module
=============================

    :Author:   Hao Zhang <quic_hazha@quicinc.com>
    :Date:     March 2023

Introduction
---------------------------

Coresight Dummy Trace Module is for the specific devices that HLOS don't
have permission to access or configure. Such as Coresight sink EUD, some
TPDMs etc. So there need driver to register dummy devices as Coresight
devices. Provide Coresight API for dummy device operations, such as
enabling and disabling dummy devices. Build the Coresight path for dummy
sink or dummy source for debugging.

Sysfs files and directories
---------------------------

Root: ``/sys/bus/coresight/devices/dummy<N>``

----

:File:            ``enable_source`` (RW)
:Notes:
    - > 0 : enable the datasets of dummy source.

    - = 0 : disable the datasets of dummy source.

:Syntax:
    ``echo 1 > enable_source``

----

:File:            ``enable_sink`` (RW)
:Notes:
    - > 0 : enable the datasets of dummy sink.

    - = 0 : disable the datasets of dummy sink.

:Syntax:
    ``echo 1 > enable_sink``

----

Config details
---------------------------

There are two types of nodes, dummy sink and dummy source. The nodes
should be observed at the coresight path
"/sys/bus/coresight/devices".
e.g.
/sys/bus/coresight/devices # ls -l | grep dummy
dummy0 -> ../../../devices/platform/soc@0/soc@0:dummy_source/dummy0
dummy1 -> ../../../devices/platform/soc@0/soc@0:dummy_sink/dummy1
