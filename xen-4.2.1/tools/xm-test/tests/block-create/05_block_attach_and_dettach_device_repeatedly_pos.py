#!/usr/bin/python

# Copyright (C) International Business Machines Corp., 2005
# Author: Murillo F. Bernardes <mfb@br.ibm.com>

import re

from XmTestLib import *
from XmTestLib.block_utils import *

if ENABLE_HVM_SUPPORT:
    SKIP("Block-attach not supported for HVM domains")

# Create a domain (default XmTestDomain, with our ramdisk)
domain = XmTestDomain()

try:
    console = domain.start()
except DomainError, e:
    if verbose:
        print "Failed to create test domain because:"
        print e.extra
    FAIL(str(e))

try:
    console.setHistorySaveCmds(value=True)
    # Run 'ls'
    run = console.runCmd("ls")
except ConsoleError, e:
    saveLog(console.getHistory())
    FAIL(str(e))
    

for i in range(10):
    block_attach(domain, "phy:ram1", "xvda1")
    run = console.runCmd("cat /proc/partitions")
    if not re.search("xvda1", run["output"]):
        FAIL("Failed to attach block device: /proc/partitions does not show that!")

    block_detach(domain, "xvda1")
    run = console.runCmd("cat /proc/partitions")
    if re.search("xvda1", run["output"]):
        FAIL("Failed to dettach block device: /proc/partitions still showing that!")

# Close the console
domain.closeConsole()

# Stop the domain (nice shutdown)
domain.stop()
