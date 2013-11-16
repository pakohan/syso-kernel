#!/bin/sh
echo "Content-type: text/html"
echo ""
echo "<h2>CPUInfo</h2>"
cat /proc/cpuinfo | sed 's/.$/<br>/g'