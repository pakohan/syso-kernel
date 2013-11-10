#!/bin/sh
echo "Content-type: text/html"
echo ""
echo "<h2>CPUInfo</h2>"
cat /proc/cpuinfo | sed '{:q;N;s/\n/<br>/g;t q}'
