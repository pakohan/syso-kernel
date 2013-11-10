#!/bin/sh
echo "Content-type: text/html"
echo ""
echo "<h2>CPUInfo</h2>"
ifconfig | sed 's/.$/<br>/g'